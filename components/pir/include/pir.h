/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when PIR motion is detected.
 */
typedef void (*pir_callback_t)(void);

/**
 * @brief Initialize the PIR sensor GPIO.
 *
 * @param gpio GPIO number connected to the PIR sensor OUT pin.
 *             Set to GPIO_NUM_NC to disable PIR support.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t pir_init(gpio_num_t gpio);

/**
 * @brief Register a callback to be called on motion detection.
 *
 * @param cb Callback function. Pass NULL to unregister.
 */
void pir_register_callback(pir_callback_t cb);

/**
 * @brief Start the PIR monitoring task.
 *
 * The task polls the GPIO and invokes the registered callback on a
 * clean rising edge (with simple debouncing).
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t pir_start(void);

#ifdef __cplusplus
}
#endif
