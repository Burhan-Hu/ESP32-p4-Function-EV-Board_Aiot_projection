/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "example_dsi_init.h"
#include "example_dsi_init_config.h"
#include "example_sensor_init.h"
#include "example_config.h"
#include "wifi_conn.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "audio.h"
#include "cloud_api.h"
#include "pir.h"
#include "camera_capture.h"
#include "human_detect.h"
#if CONFIG_WAKE_WORD_ENABLED
#include "wake_word.h"
#endif

static const char *TAG = "mipi_isp_dsi";

static esp_cam_ctlr_handle_t s_cam_handle = NULL;
static int s_cam_width = 0;
static int s_cam_height = 0;
static void *s_frame_buffer = NULL;
static size_t s_frame_buffer_size = 0;
static SemaphoreHandle_t s_cam_capture_mutex = NULL;

/* -------------------------------------------------------------------------- */
/* Continuous monitoring state machine                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    MONITOR_STATE_IDLE,
    MONITOR_STATE_ACTIVE,
} monitor_state_t;

#define MONITOR_DEFAULT_DURATION_MS (5 * 60 * 1000)
#define MONITOR_PIR_COOLDOWN_MS     5000

static monitor_state_t s_monitor_state = MONITOR_STATE_IDLE;
static uint32_t s_monitor_deadline_ms = 0;
static uint32_t s_monitor_last_pir_ms = 0;
static uint8_t *s_monitor_ref_jpeg = NULL;
static size_t s_monitor_ref_jpeg_len = 0;
static SemaphoreHandle_t s_monitor_mutex = NULL;

static void monitor_init(void)
{
    if (s_monitor_mutex == NULL) {
        s_monitor_mutex = xSemaphoreCreateMutex();
    }
}

static bool monitor_is_command(const char *text, bool *start_out)
{
    if (text == NULL || start_out == NULL) {
        return false;
    }
    if (strstr(text, "开始监测") || strstr(text, "帮我看着") ||
        strstr(text, "监控一下") || strstr(text, "开始监控")) {
        *start_out = true;
        return true;
    }
    if (strstr(text, "停止监测") || strstr(text, "结束监测") ||
        strstr(text, "停止监控") || strstr(text, "结束监控")) {
        *start_out = false;
        return true;
    }
    return false;
}

static void monitor_clear_reference(void)
{
    if (s_monitor_ref_jpeg != NULL) {
        heap_caps_free(s_monitor_ref_jpeg);
        s_monitor_ref_jpeg = NULL;
        s_monitor_ref_jpeg_len = 0;
    }
}

static void monitor_set_reference(const uint8_t *jpeg, size_t len)
{
    monitor_clear_reference();
    if (jpeg == NULL || len == 0) {
        return;
    }
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate monitor reference frame");
        return;
    }
    memcpy(copy, jpeg, len);
    s_monitor_ref_jpeg = copy;
    s_monitor_ref_jpeg_len = len;
}

static void monitor_start(void)
{
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    s_monitor_state = MONITOR_STATE_ACTIVE;
    s_monitor_deadline_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + MONITOR_DEFAULT_DURATION_MS;
    monitor_clear_reference();
    xSemaphoreGive(s_monitor_mutex);
    ESP_LOGI(TAG, "monitoring started");
}

static void monitor_stop(void)
{
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    s_monitor_state = MONITOR_STATE_IDLE;
    s_monitor_deadline_ms = 0;
    monitor_clear_reference();
    xSemaphoreGive(s_monitor_mutex);
    ESP_LOGI(TAG, "monitoring stopped");
}

static bool monitor_is_active(void)
{
    if (s_monitor_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    bool active = (s_monitor_state == MONITOR_STATE_ACTIVE);
    if (active) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now >= s_monitor_deadline_ms) {
            s_monitor_state = MONITOR_STATE_IDLE;
            monitor_clear_reference();
            active = false;
            ESP_LOGI(TAG, "monitoring timed out");
        }
    }
    xSemaphoreGive(s_monitor_mutex);
    return active;
}

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static void http_test_request(void);

static void run_vlm_tts_pipeline(const char *question, const uint8_t *jpeg_data, size_t jpeg_len);
static bool question_needs_camera(const char *text);
static bool app_human_frame_reader(uint8_t *dst, size_t dst_len, int *width, int *height, void *ctx);
static void on_human_left(uint32_t duration_ms);

/* Return true if the user question likely requires visual information. */
static bool question_needs_camera(const char *text)
{
    if (text == NULL) {
        return false;
    }

    static const char *keywords[] = {
        "看见", "看到", "画面", "摄像头", "图片", "照片",
        "描述", "这里", "这", "那", "前面", "周围", "啥", "什么",
        NULL
    };

    for (int i = 0; keywords[i] != NULL; i++) {
        if (strstr(text, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void run_vlm_tts_pipeline(const char *question, const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (question == NULL) {
        return;
    }

    cloud_response_t vlm_result = {0};
    esp_err_t ret = cloud_vlm_ask(question, jpeg_data, jpeg_len, &vlm_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VLM failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "VLM result: %s", vlm_result.text);

    int16_t *tts_pcm = NULL;
    size_t tts_samples = 0;
    ret = cloud_tts_speak(vlm_result.text, &tts_pcm, &tts_samples);
    cloud_response_free(&vlm_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "TTS generated %zu samples, playing...", tts_samples);
    audio_play_pcm(tts_pcm, tts_samples);
    cloud_pcm_free(tts_pcm);
}

static bool app_human_frame_reader(uint8_t *dst, size_t dst_len, int *width, int *height, void *ctx)
{
    (void)ctx;

    if (dst == NULL || width == NULL || height == NULL ||
        s_frame_buffer == NULL || s_cam_capture_mutex == NULL ||
        s_cam_width <= 0 || s_cam_height <= 0) {
        return false;
    }

    size_t frame_len = (size_t)s_cam_width * (size_t)s_cam_height * EXAMPLE_RGB565_BITS_PER_PIXEL / 8;
    if (dst_len < frame_len || s_frame_buffer_size < frame_len) {
        ESP_LOGW(TAG, "human frame reader buffer too small: dst=%zu frame=%zu live=%zu",
                 dst_len, frame_len, s_frame_buffer_size);
        return false;
    }

    xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
    esp_cache_msync(s_frame_buffer, frame_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(dst, s_frame_buffer, frame_len);
    xSemaphoreGive(s_cam_capture_mutex);

    *width = s_cam_width;
    *height = s_cam_height;
    return true;
}

static void on_human_left(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "human left callback, presence_duration=%" PRIu32 " ms", duration_ms);
}
#if CONFIG_WAKE_WORD_ENABLED
static void on_wake_word_detected(void)
{
    ESP_LOGI(TAG, "wake word detected callback");

    /* Record 3 seconds of audio after wake word. */
    int16_t *rec_pcm = NULL;
    size_t rec_samples = 0;
    esp_err_t ret = audio_record(3000, &rec_pcm, &rec_samples);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio record failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "recorded %zu samples for ASR", rec_samples);

    /* ASR: speech to text. */
    cloud_response_t asr_result = {0};
    ret = cloud_asr_transcribe(rec_pcm, rec_samples, &asr_result);
    audio_free_pcm(rec_pcm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "ASR result: %s", asr_result.text);
    if (asr_result.text == NULL || asr_result.text[0] == '\0') {
        ESP_LOGW(TAG, "ASR returned empty text, skipping VLM/TTS");
        cloud_response_free(&asr_result);
        return;
    }

    /* Handle monitoring start/stop commands. */
    bool start_monitor = false;
    if (monitor_is_command(asr_result.text, &start_monitor)) {
        if (start_monitor) {
            monitor_start();
            /* Capture the first frame as the reference. */
            uint8_t *ref_jpeg = NULL;
            size_t ref_len = 0;
            if (s_frame_buffer != NULL && s_cam_capture_mutex != NULL) {
                xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
                esp_err_t cap_ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                                        s_cam_width, s_cam_height,
                                                        &ref_jpeg, &ref_len);
                xSemaphoreGive(s_cam_capture_mutex);
                if (cap_ret == ESP_OK) {
                    monitor_set_reference(ref_jpeg, ref_len);
                    heap_caps_free(ref_jpeg);
                }
            }
            run_vlm_tts_pipeline("已开始监测环境，我会把检测到的变化语音告诉你。", NULL, 0);
        } else {
            monitor_stop();
            run_vlm_tts_pipeline("监测已结束。", NULL, 0);
        }
        cloud_response_free(&asr_result);
        return;
    }

    /* Capture a frame only when the question likely needs visual context. */
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    bool need_camera = question_needs_camera(asr_result.text);
    ESP_LOGI(TAG, "question needs camera: %s", need_camera ? "yes" : "no");
    if (need_camera && s_frame_buffer != NULL && s_cam_capture_mutex != NULL) {
        xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
        esp_err_t cap_ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                                s_cam_width, s_cam_height,
                                                &jpeg_buf, &jpeg_len);
        xSemaphoreGive(s_cam_capture_mutex);
        if (cap_ret != ESP_OK) {
            ESP_LOGW(TAG, "failed to capture frame for VLM");
        }
    }

    /* VLM + TTS pipeline. */
    run_vlm_tts_pipeline(asr_result.text, jpeg_buf, jpeg_len);
    cloud_response_free(&asr_result);
    if (jpeg_buf != NULL) {
        heap_caps_free(jpeg_buf);
    }

    ESP_LOGI(TAG, "interaction pipeline done");
}
#endif

#if CONFIG_PIR_SENSOR_ENABLED

/* Return true if the VLM response indicates no meaningful change. */
static bool monitor_response_is_silent(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return true;
    }
    if (strstr(text, "NO_CHANGE") != NULL) {
        return true;
    }
    if (strstr(text, "没有变化") != NULL || strstr(text, "无变化") != NULL ||
        strstr(text, "没有明显变化") != NULL || strstr(text, "无明显变化") != NULL) {
        return true;
    }
    return false;
}

static void on_pir_motion_detected(void)
{
    ESP_LOGI(TAG, "PIR motion detected callback");

    if (s_frame_buffer == NULL) {
        ESP_LOGW(TAG, "camera not ready, ignoring PIR event");
        return;
    }

    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);

    /* Check monitoring state and timeout. */
    if (s_monitor_state != MONITOR_STATE_ACTIVE) {
        xSemaphoreGive(s_monitor_mutex);
        return;
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now >= s_monitor_deadline_ms) {
        s_monitor_state = MONITOR_STATE_IDLE;
        monitor_clear_reference();
        xSemaphoreGive(s_monitor_mutex);
        run_vlm_tts_pipeline("监测已自动结束。", NULL, 0);
        return;
    }

    if (now - s_monitor_last_pir_ms < MONITOR_PIR_COOLDOWN_MS) {
        ESP_LOGI(TAG, "PIR event within monitoring cooldown, skip");
        xSemaphoreGive(s_monitor_mutex);
        return;
    }
    s_monitor_last_pir_ms = now;

    xSemaphoreGive(s_monitor_mutex);

    /* Capture current frame. */
    uint8_t *cur_jpeg = NULL;
    size_t cur_len = 0;
    xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
    esp_err_t ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                        s_cam_width, s_cam_height,
                                        &cur_jpeg, &cur_len);
    xSemaphoreGive(s_cam_capture_mutex);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor keyframe capture failed: %s", esp_err_to_name(ret));
        return;
    }

    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);

    /* First trigger: save reference frame and stay silent. */
    if (s_monitor_ref_jpeg == NULL) {
        monitor_set_reference(cur_jpeg, cur_len);
        xSemaphoreGive(s_monitor_mutex);
        heap_caps_free(cur_jpeg);
        ESP_LOGI(TAG, "monitor reference frame saved");
        return;
    }

    /* Copy reference frame data while holding the lock, so it remains valid
     * even if monitoring is stopped concurrently. */
    uint8_t *ref_jpeg = NULL;
    size_t ref_len = 0;
    if (s_monitor_ref_jpeg != NULL && s_monitor_ref_jpeg_len > 0) {
        ref_jpeg = (uint8_t *)heap_caps_malloc(s_monitor_ref_jpeg_len, MALLOC_CAP_SPIRAM);
        if (ref_jpeg != NULL) {
            memcpy(ref_jpeg, s_monitor_ref_jpeg, s_monitor_ref_jpeg_len);
            ref_len = s_monitor_ref_jpeg_len;
        }
    }
    xSemaphoreGive(s_monitor_mutex);

    if (ref_jpeg == NULL) {
        ESP_LOGE(TAG, "failed to copy reference frame for comparison");
        heap_caps_free(cur_jpeg);
        return;
    }

    /* Ask LLM to compare reference and current frames. */
    const char *prompt = "第一张图是之前的场景，第二张图是现在的场景。"
                         "请比较两张图片。如果环境没有明显变化，请只回复 'NO_CHANGE'；"
                         "如果有变化，请用一句话简要描述变化内容。";

    cloud_response_t vlm_result = {0};
    ret = cloud_vlm_ask_with_reference(prompt,
                                       ref_jpeg, ref_len,
                                       cur_jpeg, cur_len,
                                       &vlm_result);
    heap_caps_free(ref_jpeg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor VLM comparison failed: %s", esp_err_to_name(ret));
        heap_caps_free(cur_jpeg);
        return;
    }

    if (monitor_response_is_silent(vlm_result.text)) {
        ESP_LOGI(TAG, "monitor: no change detected, stay silent");
        cloud_response_free(&vlm_result);
        heap_caps_free(cur_jpeg);
        return;
    }

    ESP_LOGI(TAG, "monitor: change detected: %s", vlm_result.text);

    /* Update reference frame to current to avoid repeated alerts. */
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    monitor_set_reference(cur_jpeg, cur_len);
    xSemaphoreGive(s_monitor_mutex);
    heap_caps_free(cur_jpeg);

    /* TTS alert. */
    int16_t *tts_pcm = NULL;
    size_t tts_samples = 0;
    ret = cloud_tts_speak(vlm_result.text, &tts_pcm, &tts_samples);
    cloud_response_free(&vlm_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor TTS failed: %s", esp_err_to_name(ret));
        return;
    }
    audio_play_pcm(tts_pcm, tts_samples);
    cloud_pcm_free(tts_pcm);
}
#endif

void app_main(void)
{
    esp_err_t ret = ESP_FAIL;

    s_cam_capture_mutex = xSemaphoreCreateMutex();
    monitor_init();
    cloud_api_init();

    //---------------Wi-Fi & Network Init------------------//
    ret = wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed, continuing without network");
    } else {
        http_test_request();
    }

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
    void *frame_buffer = NULL;
    size_t frame_buffer_size = 0;

    //mipi ldo
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
        .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    /**
     * @background
     * Sensor use RAW8
     * ISP convert to RGB565
     */
    //---------------DSI Init------------------//
    example_dsi_alloc_config_t dsi_alloc_config = EXAMPLE_DSI_ALLOC_CONFIG_DEFAULT();
    example_dsi_resource_alloc(&dsi_alloc_config, &mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, &frame_buffer, NULL);
    s_frame_buffer = frame_buffer;

    //---------------Necessary variable config------------------//
    frame_buffer_size = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES * CONFIG_EXAMPLE_MIPI_DSI_DISP_VRES * EXAMPLE_RGB565_BITS_PER_PIXEL / 8;

    s_frame_buffer_size = frame_buffer_size;
    ESP_LOGD(TAG, "CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES: %d, CONFIG_EXAMPLE_MIPI_DSI_DISP_VRES: %d, bits per pixel: %d", CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES, CONFIG_EXAMPLE_MIPI_DSI_DISP_VRES, EXAMPLE_RGB565_BITS_PER_PIXEL);
    ESP_LOGD(TAG, "frame_buffer_size: %zu", frame_buffer_size);
    ESP_LOGD(TAG, "frame_buffer: %p", frame_buffer);

    esp_cam_ctlr_trans_t new_trans = {
        .buffer = frame_buffer,
        .buflen = frame_buffer_size,
    };

    //--------Camera Sensor and SCCB Init-----------//
    example_sensor_handle_t sensor_handle = {
        .sccb_handle = NULL,
        .i2c_bus_handle = NULL,
    };
    example_sensor_config_t cam_sensor_config = {
        .i2c_port_num = I2C_NUM_0,
        .i2c_sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,
        .i2c_scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name = EXAMPLE_CAM_FORMAT,
    };
    example_sensor_init(&cam_sensor_config, &sensor_handle);
    ESP_LOGI(TAG, "sensor init done");

    //---------------Audio Init------------------//
    ret = audio_init(sensor_handle.i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed, continuing without audio");
    } else {
#if CONFIG_AUDIO_ENABLE_LOOPBACK_TEST
        ESP_LOGI(TAG, "starting audio loopback test task");
        xTaskCreate(audio_loopback_test, "audio_loopback", 8192, NULL, 5, NULL);
#elif CONFIG_WAKE_WORD_ENABLED
        ESP_LOGI(TAG, "initializing wake word engine");
        ret = wake_word_init();
        if (ret == ESP_OK) {
            wake_word_register_callback(on_wake_word_detected);
            wake_word_start();
        } else {
            ESP_LOGE(TAG, "wake word init failed");
        }
#elif CONFIG_AUDIO_ENABLE_RECORD_TEST
        ESP_LOGI(TAG, "starting audio record test task");
        xTaskCreate(audio_record_test, "audio_record_test", 8192, NULL, 5, NULL);
#endif
    }

    //---------------PIR Sensor Init------------------//
#if CONFIG_PIR_SENSOR_ENABLED
    ret = pir_init((gpio_num_t)CONFIG_PIR_SENSOR_GPIO);
    if (ret == ESP_OK) {
        pir_register_callback(on_pir_motion_detected);
        pir_start();
    } else {
        ESP_LOGE(TAG, "PIR init failed");
    }
#endif

    //---------------CSI Init------------------//
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES,
        .v_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t cam_handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_config, &cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "csi init fail[%d]", ret);
        return;
    }
    s_cam_handle = cam_handle;
    s_cam_width = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES;
    s_cam_height = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES;

    /* Pre-allocate camera capture buffers so JPEG encoding does not need to
     * allocate large buffers at runtime. */
    ret = camera_capture_init(s_cam_width, s_cam_height);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera capture init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    if (esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &new_trans) != ESP_OK) {
        ESP_LOGE(TAG, "ops register fail");
        return;
    }

    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam_handle));
    ESP_LOGI(TAG, "csi enabled");

    //---------------ISP Init------------------//
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES,
        .v_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES,
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_config, &isp_proc));
    ESP_ERROR_CHECK(esp_isp_enable(isp_proc));
    ESP_LOGI(TAG, "isp enabled");

    //---------------DPI Reset------------------//
    example_dpi_panel_reset(mipi_dpi_panel);

    //init to all white
    memset(frame_buffer, 0xFF, frame_buffer_size);
    esp_cache_msync((void *)frame_buffer, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    if (esp_cam_ctlr_start(cam_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Driver start fail");
        return;
    }
    ESP_LOGI(TAG, "csi started");

    example_dpi_panel_init(mipi_dpi_panel);
    ESP_LOGI(TAG, "dpi panel init done, entering loop");

    // Only need to submit the first transaction; the callbacks will keep
    // re-submitting the same buffer for every following frame.
    ret = esp_cam_ctlr_receive(cam_handle, &new_trans, pdMS_TO_TICKS(3000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "first receive fail, ret=0x%x", ret);
        return;
    }
    ESP_LOGI(TAG, "first frame received, entering idle loop");
    ret = human_detect_init();
    if (ret == ESP_OK) {
        human_register_left_callback(on_human_left);
        ret = human_detect_register_frame_reader(app_human_frame_reader, NULL, s_cam_width, s_cam_height);
        if (ret == ESP_OK) {
            ret = human_detect_start();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "human detection start failed: %s", esp_err_to_name(ret));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#define MAX_HTTP_OUTPUT_BUFFER 1024

static void http_test_request(void)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    esp_http_client_config_t config = {
        .url = "https://httpbin.org/get",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http client open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);

    int total_read = 0;
    int r = 0;
    while (total_read < MAX_HTTP_OUTPUT_BUFFER - 1) {
        r = esp_http_client_read(client, local_response_buffer + total_read,
                                 MAX_HTTP_OUTPUT_BUFFER - 1 - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
    }
    local_response_buffer[total_read] = '\0';
    ESP_LOGI(TAG, "HTTP Response (%d bytes):\n%s", total_read, local_response_buffer);

    esp_http_client_cleanup(client);
}

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    esp_cam_ctlr_trans_t new_trans = *(esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = new_trans.buffer;
    trans->buflen = new_trans.buflen;

    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    return false;
}
