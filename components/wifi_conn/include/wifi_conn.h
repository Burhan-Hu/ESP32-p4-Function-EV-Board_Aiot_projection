/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi in STA mode and connect to the configured AP.
 *
 * This function is blocking: it returns only after the station has got an IP
 * or the connection attempt has failed.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t wifi_connect(void);

/**
 * @brief Check whether the station currently has an IP address.
 */
bool wifi_is_connected(void);

/**
 * @brief Disconnect and deinitialize Wi-Fi.
 */
esp_err_t wifi_disconnect(void);

#ifdef __cplusplus
}
#endif
