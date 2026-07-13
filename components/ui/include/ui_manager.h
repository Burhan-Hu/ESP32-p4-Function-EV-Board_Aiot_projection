#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_OVERLAY_IDLE = 0,
    UI_OVERLAY_FOCUS,
    UI_OVERLAY_SOFT_ALERT,
    UI_OVERLAY_EVENT_REVIEW,
    UI_OVERLAY_SUMMARY,

    UI_PAGE_IDLE = UI_OVERLAY_IDLE,
    UI_PAGE_FOCUS = UI_OVERLAY_FOCUS,
    UI_PAGE_ALERT = UI_OVERLAY_SOFT_ALERT,
    UI_PAGE_KEYFRAME = UI_OVERLAY_EVENT_REVIEW,
    UI_PAGE_SUMMARY = UI_OVERLAY_SUMMARY,
} ui_overlay_mode_t;

typedef ui_overlay_mode_t ui_page_t;

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_LEAVE_SEAT,
    UI_EVENT_SCREEN_SWITCH,
    UI_EVENT_PHONE_DISTRACTION,
    UI_EVENT_HEAD_DOWN,
    UI_EVENT_LYING_DESK,
    UI_EVENT_HOMEWORK_STALL,
    UI_EVENT_MULTI_PERSON,
    UI_EVENT_CAMERA_BLOCKED,
    UI_EVENT_PRIVACY_ON,
    UI_EVENT_CUSTOM,
} ui_event_type_t;

typedef struct {
    bool valid;
    /* 画面框选区域，单位是显示像素，原点在左上角。 */
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} ui_bbox_t;

typedef struct {
    ui_event_type_t type;
    const char *type_text;
    const char *message;
    /* 0 表示未知置信度，1-100 表示算法置信度。 */
    uint8_t confidence;
    int risk_level;
    uint32_t duration_seconds;
    uint32_t timestamp_seconds;
    ui_bbox_t bbox;
    bool needs_review;
} ui_event_t;

typedef struct {
    bool wifi_connected;
    bool camera_ok;
    bool privacy_on;
    int learn_score;
    int risk_level;
    const char *seat_state;
    const char *screen_state;
    const char *pose_state;
    const char *event_type;
    const char *event_message;
    const char *summary_title;
    const char *summary_text;
    uint32_t study_seconds;
    uint32_t event_duration_seconds;
    const char *homework_text;
    const char *course_title;
} ui_model_t;

/* UI 模块只消费状态，不负责算法、网络或摄像头采集。 */
void ui_manager_init(void);
void ui_manager_show_page(ui_page_t page);
void ui_manager_update(const ui_model_t *model);
void ui_manager_push_event(const ui_event_t *event);
void ui_manager_clear_event(void);
void ui_manager_show_alert(const char *event_type, const char *message);
void ui_manager_show_summary(const char *title, const char *summary);
void ui_manager_get_snapshot(ui_model_t *model, ui_event_t *event, ui_overlay_mode_t *mode);

#ifdef __cplusplus
}
#endif
