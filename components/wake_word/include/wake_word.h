/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when the wake word is detected.
 */
typedef void (*wake_word_callback_t)(void);

/**
 * @brief Initialize the WakeNet wake word engine.
 *
 * The selected model is controlled by the ESP Speech Recognition Kconfig
 * options from the esp-sr component.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t wake_word_init(void);

/**
 * @brief Start the wake word detection task.
 *
 * The task continuously reads audio from the codec and runs WakeNet
 * inference. When a wake word is detected, the registered callback is
 * invoked.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t wake_word_start(void);

/**
 * @brief Register a callback to be called on wake word detection.
 *
 * @param cb Callback function. Pass NULL to unregister.
 */
void wake_word_register_callback(wake_word_callback_t cb);

#ifdef __cplusplus
}
#endif
