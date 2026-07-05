/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perform ESP-Hosted C6 slave OTA from the 'slave_fw' host partition.
 *
 * Reads the slave firmware image from the dedicated flash partition and pushes
 * it to the co-processor over the existing SDIO/SPI transport.
 *
 * If the OTA completes, the host is restarted so that both host and slave
 * re-initialise with the new slave firmware.
 *
 * @return ESP_OK if OTA is not required (already up to date) or completed.
 *         Error code if OTA failed.
 */
esp_err_t slave_ota_perform_if_needed(void);

#ifdef __cplusplus
}
#endif
