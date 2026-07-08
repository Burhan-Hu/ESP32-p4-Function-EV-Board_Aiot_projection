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
 * @brief Initialize the camera capture module.
 *
 * Pre-allocates internal buffers based on the expected frame resolution.
 * Must be called once before using camera_capture_jpeg().
 *
 * @param width  Frame width in pixels.
 * @param height Frame height in pixels.
 * @return ESP_OK on success.
 */
esp_err_t camera_capture_init(int width, int height);

/**
 * @brief Encode one RGB565 camera frame as JPEG.
 *
 * The input buffer is copied internally before encoding, so the caller can keep
 * using the live frame buffer while encoding happens. The caller must free the
 * returned JPEG buffer with heap_caps_free().
 *
 * @param rgb565_buf    Source RGB565 frame buffer (read-only).
 * @param width         Frame width in pixels.
 * @param height        Frame height in pixels.
 * @param jpeg_out      Output pointer to the JPEG buffer.
 * @param jpeg_len_out  Output JPEG buffer length.
 * @return ESP_OK on success.
 */
esp_err_t camera_capture_jpeg(const uint8_t *rgb565_buf,
                              int width, int height,
                              uint8_t **jpeg_out, size_t *jpeg_len_out);

#ifdef __cplusplus
}
#endif
