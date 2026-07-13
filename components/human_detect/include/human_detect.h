/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy one RGB565 frame into dst. Return true when a fresh frame was copied. */
typedef bool (*human_frame_reader_t)(uint8_t *dst,
                                     size_t dst_len,
                                     int *width,
                                     int *height,
                                     void *ctx);

/* Person-left callback. duration_ms is the last continuous presence duration. */
typedef void (*human_left_callback_t)(uint32_t duration_ms);

/* Person-present callback: fired once when a person is confirmed present. */
typedef void (*human_present_callback_t)(void);

/* Person-left reminder callback: fired once after the person has been absent
 * for CONFIG_HUMAN_DETECT_LEAVE_REMINDER_MS milliseconds. absent_ms is the
 * actual absence duration at the moment of the call. */
typedef void (*human_left_reminder_callback_t)(uint32_t absent_ms);

/* Initialize ESP-WHO pedestrian detection and LED linkage. */
int human_detect_init(void);

/* Register the RGB565 frame source used by the detection task. */
int human_detect_register_frame_reader(human_frame_reader_t reader,
                                       void *ctx,
                                       int width,
                                       int height);

/* Start the background human detection task. */
int human_detect_start(void);

/* Current debounced person-present state. */
bool human_is_present(void);

/* Set person-left timeout in milliseconds. */
void human_set_leave_timeout(uint32_t ms);

/* Register a person-left callback. Pass NULL to unregister. */
void human_register_left_callback(human_left_callback_t cb);

/* Register a person-present callback. Pass NULL to unregister. */
void human_register_present_callback(human_present_callback_t cb);

/* Register a one-shot person-left reminder callback. Pass NULL to unregister. */
void human_register_left_reminder_callback(human_left_reminder_callback_t cb);

#ifdef __cplusplus
}
#endif
