/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the ES8311 audio codec and I2S interface.
 *
 * Reuses an existing I2C master bus for codec control (ES8311 shares the
 * camera SCCB bus on the ESP32-P4-Function-EV-Board) and configures I2S for
 * audio data transfer.
 *
 * @param i2c_bus_handle Handle to an already initialized I2C master bus.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus_handle);

/**
 * @brief Get the codec device handle.
 *
 * This handle can be used by other components (e.g. wake word engine) that
 * need to read audio samples directly from the codec.
 *
 * @return Codec device handle, or NULL if audio_init() has not been called.
 */
void *audio_get_codec_handle(void);

/**
 * @brief Lock the codec device for exclusive access.
 *
 * Any code reading from or writing to the codec (e.g. wake word task,
 * audio_record()) must hold this lock to avoid interleaved samples.
 */
void audio_codec_lock(void);

/**
 * @brief Unlock the codec device.
 */
void audio_codec_unlock(void);

/**
 * @brief Record mono 16-bit 16 kHz PCM audio from the microphone.
 *
 * The function allocates a DMA-capable buffer internally, reads stereo data
 * from the codec, and returns a mono buffer. The caller must release the
 * returned buffer with audio_free_pcm().
 *
 * @param duration_ms   Recording duration in milliseconds.
 * @param pcm_out       Output pointer to the mono PCM buffer.
 * @param sample_count  Output number of mono 16-bit samples.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t audio_record(int duration_ms, int16_t **pcm_out, size_t *sample_count);

/**
 * @brief Play mono 16-bit 16 kHz PCM audio through the speaker.
 *
 * The input mono samples are duplicated to stereo internally before being
 * sent to the codec.
 *
 * @param pcm           Pointer to mono PCM samples.
 * @param sample_count  Number of mono 16-bit samples.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t audio_play_pcm(const int16_t *pcm, size_t sample_count);

/**
 * @brief Free a PCM buffer returned by audio_record().
 *
 * @param pcm Buffer returned from audio_record(), may be NULL.
 */
void audio_free_pcm(int16_t *pcm);

/**
 * @brief Run an audio record-only test.
 *
 * Records audio from the on-board microphone for a fixed duration and prints
 * the peak amplitude. Useful when no speaker is connected yet.
 *
 * @param arg FreeRTOS task argument (unused).
 */
void audio_record_test(void *arg);

/**
 * @brief Run a short audio loopback test.
 *
 * Records audio from the on-board microphone and plays it back through the
 * speaker output. This function blocks indefinitely; use it only for testing.
 *
 * @param arg FreeRTOS task argument (unused).
 */
void audio_loopback_test(void *arg);

#ifdef __cplusplus
}
#endif
