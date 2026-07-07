/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Text response from cloud ASR/VLM APIs.
 *
 * The caller must release @c text with cloud_response_free().
 */
typedef struct {
    char *text;
    size_t len;
} cloud_response_t;

/**
 * @brief Send mono 16-bit 16 kHz PCM to ASR and get transcribed text.
 *
 * @param pcm           Input PCM samples.
 * @param sample_count  Number of samples.
 * @param out           Output text response.
 * @return ESP_OK on success.
 */
esp_err_t cloud_asr_transcribe(const int16_t *pcm, size_t sample_count,
                               cloud_response_t *out);

/**
 * @brief Send a JPEG image and a question to a cloud VLM.
 *
 * @param question   User question (UTF-8).
 * @param jpeg_data  JPEG image buffer.
 * @param jpeg_len   JPEG buffer length.
 * @param out        Output answer text.
 * @return ESP_OK on success.
 */
esp_err_t cloud_vlm_ask(const char *question,
                        const uint8_t *jpeg_data, size_t jpeg_len,
                        cloud_response_t *out);

/**
 * @brief Convert text to mono 16-bit 16 kHz PCM speech.
 *
 * @param text            Input text (UTF-8).
 * @param pcm_out         Output PCM buffer.
 * @param sample_count_out Output sample count.
 * @return ESP_OK on success.
 */
esp_err_t cloud_tts_speak(const char *text,
                          int16_t **pcm_out, size_t *sample_count_out);

/**
 * @brief Free a text response returned by ASR/VLM.
 */
void cloud_response_free(cloud_response_t *resp);

/**
 * @brief Free a PCM buffer returned by TTS.
 */
void cloud_pcm_free(int16_t *pcm);

#ifdef __cplusplus
}
#endif
