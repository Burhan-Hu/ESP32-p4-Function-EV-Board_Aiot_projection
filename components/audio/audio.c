/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "sdkconfig.h"
#include "audio.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_BITS              I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_CHANNELS          2
#define AUDIO_MCLK_MULTIPLE     384
#define AUDIO_LOOPBACK_BUF_SIZE (16000 * sizeof(int16_t) * 2) /* 0.5s stereo @ 16kHz */
#define AUDIO_RECORD_MAX_MS     5000

static int16_t *s_record_stereo_buf = NULL;
static int16_t *s_record_mono_buf = NULL;
static SemaphoreHandle_t s_record_buf_mutex = NULL;

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static esp_codec_dev_handle_t s_codec_handle = NULL;

static esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUDIO_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_AUDIO_I2S_MCLK_GPIO,
            .bclk = CONFIG_AUDIO_I2S_BCLK_GPIO,
            .ws   = CONFIG_AUDIO_I2S_WS_GPIO,
            .dout = CONFIG_AUDIO_I2S_DOUT_GPIO,
            .din  = CONFIG_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "i2s rx enable failed");

    return ESP_OK;
}

static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static SemaphoreHandle_t s_codec_mutex = NULL;

static esp_err_t audio_codec_init(i2c_master_bus_handle_t i2c_bus_handle)
{
    s_i2c_bus_handle = i2c_bus_handle;

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2c ctrl");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2s data");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec gpio");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = CONFIG_AUDIO_I2S_MCLK_GPIO >= 0,
        .pa_pin = CONFIG_AUDIO_PA_CTRL_GPIO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (es8311_if == NULL) {
        ESP_LOGE(TAG, "failed to create es8311 interface");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    s_codec_handle = esp_codec_dev_new(&dev_cfg);
    if (s_codec_handle == NULL) {
        ESP_LOGE(TAG, "failed to create codec device");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = AUDIO_BITS,
        .channel = AUDIO_CHANNELS,
        .channel_mask = 0x03,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec_handle, &sample_cfg), TAG, "codec open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec_handle, CONFIG_AUDIO_PLAYBACK_VOLUME), TAG, "set volume failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_codec_handle, CONFIG_AUDIO_MIC_GAIN), TAG, "set mic gain failed");

    return ESP_OK;
}

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus_handle)
{
    ESP_LOGI(TAG, "Initializing audio (ES8311 @ shared I2C bus, I2S%d)...", I2S_NUM_0);
    ESP_RETURN_ON_ERROR(audio_i2s_init(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(audio_codec_init(i2c_bus_handle), TAG, "codec init failed");

    s_codec_mutex = xSemaphoreCreateMutex();
    if (s_codec_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create codec mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Pre-allocate recording buffers to avoid runtime fragmentation. */
    s_record_buf_mutex = xSemaphoreCreateMutex();
    if (s_record_buf_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create record buffer mutex");
        return ESP_ERR_NO_MEM;
    }

    const size_t max_stereo_samples = (AUDIO_SAMPLE_RATE * AUDIO_RECORD_MAX_MS) / 1000;
    s_record_stereo_buf = (int16_t *)heap_caps_malloc(max_stereo_samples * sizeof(int16_t) * AUDIO_CHANNELS,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_record_stereo_buf == NULL) {
        ESP_LOGE(TAG, "failed to pre-allocate stereo record buffer");
        return ESP_ERR_NO_MEM;
    }

    s_record_mono_buf = (int16_t *)heap_caps_malloc(max_stereo_samples * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM);
    if (s_record_mono_buf == NULL) {
        ESP_LOGE(TAG, "failed to pre-allocate mono record buffer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio initialized successfully");
    return ESP_OK;
}

void *audio_get_codec_handle(void)
{
    return (void *)s_codec_handle;
}

void audio_codec_lock(void)
{
    if (s_codec_mutex != NULL) {
        xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    }
}

void audio_codec_unlock(void)
{
    if (s_codec_mutex != NULL) {
        xSemaphoreGive(s_codec_mutex);
    }
}

esp_err_t audio_record(int duration_ms, int16_t **pcm_out, size_t *sample_count)
{
    if (s_codec_handle == NULL || pcm_out == NULL || sample_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms <= 0 || duration_ms > AUDIO_RECORD_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_record_stereo_buf == NULL || s_record_mono_buf == NULL) {
        ESP_LOGE(TAG, "record buffers not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t mono_samples = (size_t)((AUDIO_SAMPLE_RATE * duration_ms) / 1000);
    const size_t stereo_samples = mono_samples * AUDIO_CHANNELS;
    const size_t stereo_bytes = stereo_samples * sizeof(int16_t);

    xSemaphoreTake(s_record_buf_mutex, portMAX_DELAY);

    audio_codec_lock();
    int ret = esp_codec_dev_read(s_codec_handle, s_record_stereo_buf, (int)stereo_bytes);
    audio_codec_unlock();
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec read failed: %d", ret);
        xSemaphoreGive(s_record_buf_mutex);
        return ESP_FAIL;
    }

    /* Extract left channel from interleaved stereo data. */
    int32_t peak = 0;
    for (size_t i = 0; i < mono_samples; i++) {
        s_record_mono_buf[i] = s_record_stereo_buf[i * AUDIO_CHANNELS];
        int32_t v = s_record_mono_buf[i] >= 0 ? s_record_mono_buf[i] : -s_record_mono_buf[i];
        if (v > peak) {
            peak = v;
        }
    }

    /* Copy the captured mono data into a caller-owned buffer in PSRAM. */
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(mono_samples * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM);
    if (mono_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate mono record buffer");
        xSemaphoreGive(s_record_buf_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(mono_buf, s_record_mono_buf, mono_samples * sizeof(int16_t));
    xSemaphoreGive(s_record_buf_mutex);

    *pcm_out = mono_buf;
    *sample_count = mono_samples;
    ESP_LOGI(TAG, "recorded %zu mono samples (%d ms), peak=%ld", mono_samples, duration_ms, peak);
    return ESP_OK;
}

esp_err_t audio_play_pcm(const int16_t *pcm, size_t sample_count)
{
    if (s_codec_handle == NULL || pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count == 0) {
        return ESP_OK;
    }

    /* Stream playback using a small fixed-size DMA buffer to avoid large
     * allocations for long TTS responses. */
    const size_t chunk_mono_samples = 1024;
    const size_t chunk_stereo_samples = chunk_mono_samples * AUDIO_CHANNELS;
    const size_t chunk_stereo_bytes = chunk_stereo_samples * sizeof(int16_t);

    int16_t *stereo_buf = (int16_t *)heap_caps_malloc(chunk_stereo_bytes,
                                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (stereo_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate playback chunk buffer");
        return ESP_ERR_NO_MEM;
    }

    audio_codec_lock();
    size_t offset = 0;
    while (offset < sample_count) {
        size_t remaining = sample_count - offset;
        size_t cur_mono = (remaining < chunk_mono_samples) ? remaining : chunk_mono_samples;
        size_t cur_stereo_bytes = cur_mono * AUDIO_CHANNELS * sizeof(int16_t);

        for (size_t i = 0; i < cur_mono; i++) {
            /* Amplify by 2x ("放大1倍") and clamp to int16 range. */
            int32_t amplified = (int32_t)pcm[offset + i] * 2;
            if (amplified > INT16_MAX) {
                amplified = INT16_MAX;
            } else if (amplified < INT16_MIN) {
                amplified = INT16_MIN;
            }
            stereo_buf[i * AUDIO_CHANNELS]     = (int16_t)amplified;
            stereo_buf[i * AUDIO_CHANNELS + 1] = (int16_t)amplified;
        }

        int ret = esp_codec_dev_write(s_codec_handle, stereo_buf, (int)cur_stereo_bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            audio_codec_unlock();
            heap_caps_free(stereo_buf);
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            return ESP_FAIL;
        }
        offset += cur_mono;
    }
    audio_codec_unlock();
    heap_caps_free(stereo_buf);
    return ESP_OK;
}

void audio_free_pcm(int16_t *pcm)
{
    if (pcm != NULL) {
        heap_caps_free(pcm);
    }
}

static int32_t find_peak(const int16_t *samples, size_t count)
{
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

void audio_record_test(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Audio record test started (record 3s -> print peak)");
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        int16_t *pcm = NULL;
        size_t samples = 0;
        esp_err_t ret = audio_record(3000, &pcm, &samples);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int32_t peak = find_peak(pcm, samples);
        ESP_LOGI(TAG, "record test: %zu mono samples, peak=%ld (max=%d)",
                 samples, peak, (int)INT16_MAX);
        audio_free_pcm(pcm);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void audio_loopback_test(void *arg)
{
    (void)arg;
    int16_t *buffer = (int16_t *)heap_caps_malloc(AUDIO_LOOPBACK_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate loopback buffer");
        return;
    }

    const size_t sample_count = AUDIO_LOOPBACK_BUF_SIZE / sizeof(int16_t);
    ESP_LOGI(TAG, "Audio loopback test started (record 0.5s -> playback)");
    while (true) {
        memset(buffer, 0, AUDIO_LOOPBACK_BUF_SIZE);
        int ret = esp_codec_dev_read(s_codec_handle, buffer, AUDIO_LOOPBACK_BUF_SIZE);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t peak = find_peak(buffer, sample_count);
        ESP_LOGI(TAG, "loopback: peak=%ld (silence=0, max=%d)", peak, (int)INT16_MAX);

        ret = esp_codec_dev_write(s_codec_handle, buffer, AUDIO_LOOPBACK_BUF_SIZE);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
    }
}
