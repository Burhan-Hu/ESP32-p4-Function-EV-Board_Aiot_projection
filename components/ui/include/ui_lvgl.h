#pragma once

#include "ui_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the LVGL overlay layer.
 *
 * @param display_buf  The buffer that the DPI panel scans out (RGB565).
 * @param cam_buf      The buffer where the ISP/CSI writes the camera frame (RGB565).
 * @param width        Horizontal resolution.
 * @param height       Vertical resolution.
 */
void ui_lvgl_init(void *display_buf, void *cam_buf, int width, int height);

/**
 * @brief Notify the overlay task that a new camera frame is ready.
 *
 * Safe to call from an ISR context.
 */
void ui_lvgl_notify_frame_ready(BaseType_t *pxHigherPriorityTaskWoken);

/**
 * @brief Update the model/event that the overlay should display.
 *
 * The actual LVGL rendering happens in the overlay task.
 */
void ui_lvgl_update(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event);

#ifdef __cplusplus
}
#endif
