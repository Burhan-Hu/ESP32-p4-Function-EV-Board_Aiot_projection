/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "audio.h"
#include "wake_word.h"

static const char *TAG = "wake_word";

static wake_word_callback_t s_wake_cb = NULL;
static const esp_wn_iface_t *s_wakenet = NULL;
static model_iface_data_t *s_model_data = NULL;
static srmodel_list_t *s_srmodels = NULL;

void wake_word_register_callback(wake_word_callback_t cb)
{
    s_wake_cb = cb;
}

static void wake_word_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t codec = (esp_codec_dev_handle_t)audio_get_codec_handle();
    if (codec == NULL) {
        ESP_LOGE(TAG, "codec not initialized");
        vTaskDelete(NULL);
        return;
    }

    int chunk_samples = s_wakenet->get_samp_chunksize(s_model_data);
    if (chunk_samples <= 0) {
        ESP_LOGE(TAG, "invalid wake word chunk size: %d", chunk_samples);
        vTaskDelete(NULL);
        return;
    }

    int16_t *buffer = (int16_t *)heap_caps_malloc(chunk_samples * sizeof(int16_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate wake word buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "wake word task started, chunk_samples=%d", chunk_samples);

    while (true) {
        memset(buffer, 0, chunk_samples * sizeof(int16_t));
        int ret = esp_codec_dev_read(codec, buffer, chunk_samples * sizeof(int16_t));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        wakenet_state_t state = s_wakenet->detect(s_model_data, buffer);
        if (state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected");
            if (s_wake_cb != NULL) {
                s_wake_cb();
            }
        }
    }
}

esp_err_t wake_word_init(void)
{
    ESP_LOGI(TAG, "initializing WakeNet");

    s_srmodels = esp_srmodel_init("model");
    if (s_srmodels == NULL) {
        ESP_LOGE(TAG, "failed to init srmodel list (model partition missing?)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "loaded %d model(s), WN prefix='%s'", s_srmodels->num, ESP_WN_PREFIX);
    for (int i = 0; i < s_srmodels->num; i++) {
        ESP_LOGI(TAG, "  model[%d]: %s (%s)", i,
                 s_srmodels->model_name[i],
                 s_srmodels->model_info[i] ? s_srmodels->model_info[i] : "");
    }

    char *model_name = esp_srmodel_filter(s_srmodels, ESP_WN_PREFIX, NULL);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "no wake word model found in model partition");
        esp_srmodel_deinit(s_srmodels);
        s_srmodels = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "using wake word model: %s", model_name);

    s_wakenet = esp_wn_handle_from_name(model_name);
    if (s_wakenet == NULL) {
        ESP_LOGE(TAG, "failed to get wakenet handle for %s", model_name);
        esp_srmodel_deinit(s_srmodels);
        s_srmodels = NULL;
        return ESP_FAIL;
    }

    s_model_data = s_wakenet->create(model_name, DET_MODE_95);
    if (s_model_data == NULL) {
        ESP_LOGE(TAG, "failed to create wake word model");
        esp_srmodel_deinit(s_srmodels);
        s_srmodels = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wake word model created");
    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (s_wakenet == NULL || s_model_data == NULL) {
        ESP_LOGE(TAG, "wake word not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(wake_word_task, "wake_word",
                                 CONFIG_WAKE_WORD_TASK_STACK_SIZE,
                                 NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create wake word task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
