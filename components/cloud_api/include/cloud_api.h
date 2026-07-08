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
 * @brief Initialize the cloud API module.
 *
 * Must be called once before using ASR/VLM/TTS. Safe to call multiple times.
 */
void cloud_api_init(void);

/**
 * @brief Clear the VLM conversation history.
 */
void cloud_vlm_history_clear(void);

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
 * @brief Send two JPEG images (reference + current) and a question to a cloud VLM.
 *
 * Typically used for change detection: the LLM compares the two images and
 * describes what changed. The caller must free @c out with cloud_response_free().
 *
 * @param question       User question (UTF-8).
 * @param ref_jpeg_data  Reference JPEG image buffer.
 * @param ref_jpeg_len   Reference JPEG buffer length.
 * @param cur_jpeg_data  Current JPEG image buffer.
 * @param cur_jpeg_len   Current JPEG buffer length.
 * @param out            Output answer text.
 * @return ESP_OK on success.
 */
esp_err_t cloud_vlm_ask_with_reference(const char *question,
                                       const uint8_t *ref_jpeg_data, size_t ref_jpeg_len,
                                       const uint8_t *cur_jpeg_data, size_t cur_jpeg_len,
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
