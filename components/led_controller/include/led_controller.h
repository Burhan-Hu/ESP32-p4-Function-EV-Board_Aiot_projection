/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize LED PWM output. */
int led_controller_init(void);

/* Turn LED on using the current/default brightness. */
void led_turn_on(void);

/* Turn LED off. */
void led_turn_off(void);

/* Set brightness in percent, clamped to 0..100. */
void led_set_brightness(int percent);

/* Query the logical LED state. */
bool led_is_on(void);

#ifdef __cplusplus
}
#endif
