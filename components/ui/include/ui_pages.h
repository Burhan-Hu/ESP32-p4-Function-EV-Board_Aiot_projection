#pragma once

#include "ui_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *ui_page_name(ui_page_t page);
const char *ui_overlay_name(ui_overlay_mode_t mode);
const char *ui_event_type_name(ui_event_type_t type);
void ui_pages_render(ui_page_t page, const ui_model_t *model);
void ui_pages_render_overlay(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event);

#ifdef __cplusplus
}
#endif
