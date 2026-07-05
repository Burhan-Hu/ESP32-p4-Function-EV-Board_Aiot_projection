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
