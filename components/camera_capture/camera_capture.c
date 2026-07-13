/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"
#include "driver/jpeg_types.h"
#include "camera_capture.h"

static const char *TAG = "camera_capture";

#define JPEG_OUTPUT_BUF_SIZE (1024 * 1024) /* 1 MB should be enough for 1024x600 */

static uint8_t *s_rgb_buf = NULL;
static size_t s_rgb_buf_size = 0;
static uint8_t *s_jpeg_buf = NULL;
static size_t s_jpeg_buf_size = 0;
static SemaphoreHandle_t s_capture_mutex = NULL;
static int s_cap_width = 0;
static int s_cap_height = 0;

esp_err_t camera_capture_init(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_capture_mutex == NULL) {
        s_capture_mutex = xSemaphoreCreateMutex();
        if (s_capture_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_capture_mutex, portMAX_DELAY);

    s_cap_width = width;
    s_cap_height = height;
    s_rgb_buf_size = (size_t)width * height * 2;
    s_jpeg_buf_size = JPEG_OUTPUT_BUF_SIZE;

    if (s_rgb_buf != NULL) {
        heap_caps_free(s_rgb_buf);
        s_rgb_buf = NULL;
    }
    if (s_jpeg_buf != NULL) {
        heap_caps_free(s_jpeg_buf);
        s_jpeg_buf = NULL;
    }

    /* Keep capture buffers in PSRAM to preserve internal RAM.
     * Use jpeg_alloc_encoder_mem() so that the JPEG engine's alignment
     * requirements (cache-line alignment for the output bit-stream) are met. */
    jpeg_encode_memory_alloc_cfg_t input_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    s_rgb_buf = (uint8_t *)jpeg_alloc_encoder_mem(s_rgb_buf_size, &input_mem_cfg, &s_rgb_buf_size);
    if (s_rgb_buf == NULL) {
        ESP_LOGE(TAG, "failed to pre-allocate RGB565 capture buffer");
        xSemaphoreGive(s_capture_mutex);
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_memory_alloc_cfg_t output_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    s_jpeg_buf = (uint8_t *)jpeg_alloc_encoder_mem(s_jpeg_buf_size, &output_mem_cfg, &s_jpeg_buf_size);
    if (s_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "failed to pre-allocate JPEG capture buffer");
        heap_caps_free(s_rgb_buf);
        s_rgb_buf = NULL;
        xSemaphoreGive(s_capture_mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_capture_mutex);
    ESP_LOGI(TAG, "capture buffers pre-allocated: %dx%d, rgb=%zu, jpeg=%zu",
             width, height, s_rgb_buf_size, s_jpeg_buf_size);
    return ESP_OK;
}

esp_err_t camera_capture_jpeg(const uint8_t *rgb565_buf,
                              int width, int height,
                              uint8_t **jpeg_out, size_t *jpeg_len_out)
{
    if (rgb565_buf == NULL || jpeg_out == NULL || jpeg_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_capture_mutex == NULL || s_rgb_buf == NULL || s_jpeg_buf == NULL) {
        ESP_LOGE(TAG, "capture module not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    *jpeg_out = NULL;
    *jpeg_len_out = 0;

    const size_t rgb565_size = (size_t)width * height * 2;
    if (rgb565_size > s_rgb_buf_size) {
        ESP_LOGE(TAG, "frame too large for pre-allocated buffer");
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_capture_mutex, portMAX_DELAY);

    /* Sync cache so the CPU sees the latest DMA-written frame data. */
    esp_cache_msync((void *)rgb565_buf, rgb565_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(s_rgb_buf, rgb565_buf, rgb565_size);

    /* Create a one-shot JPEG encoder. */
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    jpeg_encoder_handle_t encoder = NULL;
    esp_err_t ret = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create JPEG encoder: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_capture_mutex);
        return ret;
    }

    jpeg_encode_cfg_t encode_cfg = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 80,
    };

    uint32_t jpeg_size = 0;
    ret = jpeg_encoder_process(encoder, &encode_cfg,
                               s_rgb_buf, (uint32_t)rgb565_size,
                               s_jpeg_buf, (uint32_t)s_jpeg_buf_size,
                               &jpeg_size);
    jpeg_del_encoder_engine(encoder);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_capture_mutex);
        return ret;
    }

    if (jpeg_size == 0 || jpeg_size > s_jpeg_buf_size) {
        ESP_LOGE(TAG, "JPEG encode returned invalid size: %lu", (unsigned long)jpeg_size);
        xSemaphoreGive(s_capture_mutex);
        return ESP_FAIL;
    }

    /* Copy the encoded JPEG into a caller-owned buffer in PSRAM. */
    uint8_t *jpeg_copy = (uint8_t *)heap_caps_malloc(jpeg_size, MALLOC_CAP_SPIRAM);
    if (jpeg_copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate JPEG output copy");
        xSemaphoreGive(s_capture_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(jpeg_copy, s_jpeg_buf, jpeg_size);
    xSemaphoreGive(s_capture_mutex);

    *jpeg_out = jpeg_copy;
    *jpeg_len_out = (size_t)jpeg_size;
    ESP_LOGI(TAG, "captured %dx%d JPEG, size=%zu", width, height, *jpeg_len_out);
    return ESP_OK;
}
