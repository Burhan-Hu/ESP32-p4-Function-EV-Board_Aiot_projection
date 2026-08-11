/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REID_FEATURE_DIM 128
#define REID_INPUT_H     128
#define REID_INPUT_W     64

/**
 * @brief Initialize the OSNet upper-body Re-ID model.
 *
 * Loads the .espdl model from the "reid_model" flash partition.
 *
 * @return 0 on success, non-zero on error.
 */
int reid_model_init(void);

/**
 * @brief Extract a 128-d L2-normalized feature from a frame region.
 *
 * The region is cropped, resized to 128x64, normalized with ImageNet mean/std,
 * and fed into the OSNet model.
 *
 * @param rgb565_buf  Source RGB565 frame buffer.
 * @param frame_w     Frame width in pixels.
 * @param frame_h     Frame height in pixels.
 * @param x1          Left of the person bounding box.
 * @param y1          Top of the person bounding box.
 * @param x2          Right of the person bounding box (exclusive).
 * @param y2          Bottom of the person bounding box (exclusive).
 * @param feature_out Output buffer of REID_FEATURE_DIM floats.
 * @return true on success, false on failure.
 */
bool reid_model_extract(const uint8_t *rgb565_buf,
                        int frame_w, int frame_h,
                        int x1, int y1, int x2, int y2,
                        float *feature_out);

/**
 * @brief Compute cosine similarity between two L2-normalized features.
 */
float reid_cosine_similarity(const float *a, const float *b);

#ifdef __cplusplus
}
#endif
