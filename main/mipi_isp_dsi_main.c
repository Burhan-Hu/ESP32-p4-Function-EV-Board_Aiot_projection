/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
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
#if CONFIG_WAKE_WORD_ENABLED
#include "wake_word.h"
#endif

static const char *TAG = "mipi_isp_dsi";

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static void http_test_request(void);

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

    /* VLM: image question answering (mock uses NULL image for now). */
    cloud_response_t vlm_result = {0};
    ret = cloud_vlm_ask(asr_result.text, NULL, 0, &vlm_result);
    cloud_response_free(&asr_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VLM failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "VLM result: %s", vlm_result.text);

    /* TTS: text to speech and play. */
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

    ESP_LOGI(TAG, "interaction pipeline done");
}
#endif

void app_main(void)
{
    esp_err_t ret = ESP_FAIL;

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
    example_dsi_resource_alloc(&mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, &frame_buffer);

    //---------------Necessary variable config------------------//
    frame_buffer_size = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES * CONFIG_EXAMPLE_MIPI_DSI_DISP_VRES * EXAMPLE_RGB565_BITS_PER_PIXEL / 8;

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
        .user_data = local_response_buffer,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
        ESP_LOGI(TAG, "HTTP Response:\n%s", local_response_buffer);
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

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
