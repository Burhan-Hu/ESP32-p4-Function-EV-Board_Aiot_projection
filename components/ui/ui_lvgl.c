#include "ui_lvgl.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "lvgl.h"
#include "ui_pages.h"

static const char *TAG = "guardian_ui_lvgl";

#define UI_LVGL_TASK_STACK_SIZE 16384
#define UI_LVGL_TASK_PRIORITY   5

static void *s_display_buf = NULL;
static void *s_cam_buf = NULL;
static int s_width = 0;
static int s_height = 0;
static int s_ui_width = 0;
static int s_cam_width = 0;

static SemaphoreHandle_t s_state_lock = NULL;
static SemaphoreHandle_t s_frame_ready_sem = NULL;
static SemaphoreHandle_t s_update_sem = NULL;

static ui_overlay_mode_t s_mode = UI_OVERLAY_IDLE;
static ui_model_t s_model = {0};
static ui_event_t s_event = {0};
static bool s_model_updated = false;

static lv_display_t *s_disp = NULL;

/* Top bar */
static lv_obj_t *s_top_bar = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_cam_label = NULL;

/* Idle / focus shared widgets */
static lv_obj_t *s_face_container = NULL;
static lv_obj_t *s_face_eye_l = NULL;
static lv_obj_t *s_face_eye_r = NULL;
static lv_obj_t *s_face_mouth = NULL;
static lv_obj_t *s_face_label = NULL;

static lv_obj_t *s_timer_label = NULL;

/* Status row */
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_seat_label = NULL;
static lv_obj_t *s_pose_label = NULL;
static lv_obj_t *s_screen_label = NULL;

/* Alert */
static lv_obj_t *s_alert_bar = NULL;
static lv_obj_t *s_alert_title = NULL;
static lv_obj_t *s_alert_msg = NULL;

/* Summary */
static lv_obj_t *s_summary_panel = NULL;
static lv_obj_t *s_summary_title = NULL;
static lv_obj_t *s_summary_text = NULL;

/* Custom font generated from all CJK characters used in the UI.
 * See tools_temp/gen_ui_font.py
 */
LV_FONT_DECLARE(ui_font_custom);

static const lv_font_t *ui_font(void)
{
    return &ui_font_custom;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;

    if (s_display_buf) {
        int32_t w = lv_display_get_horizontal_resolution(disp);
        int32_t h = lv_display_get_vertical_resolution(disp);
        esp_cache_msync(s_display_buf, (size_t)w * h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    lv_display_flush_ready(disp);
}

static void set_container_style(lv_obj_t *obj, lv_color_t bg, lv_opa_t opa)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, ui_font(), 0);
    return lbl;
}

static void update_top_bar(void)
{
    lv_label_set_text_fmt(s_title_label, "智学 Guardian  %s",
                          s_model.privacy_on ? "[隐私模式]" : "");
    lv_label_set_text_fmt(s_wifi_label, "Wi-Fi:%s", s_model.wifi_connected ? "已连" : "未连");
    lv_label_set_text_fmt(s_cam_label, "摄像头:%s", s_model.camera_ok ? "正常" : "未就绪");
}

static void update_status_row(void)
{
    lv_label_set_text_fmt(s_seat_label, "在座\n%s",
                          s_model.seat_state && s_model.seat_state[0] ? s_model.seat_state : "检测中");
    lv_label_set_text_fmt(s_pose_label, "姿态\n%s",
                          s_model.pose_state && s_model.pose_state[0] ? s_model.pose_state : "检测中");
    lv_label_set_text_fmt(s_screen_label, "页面\n%s",
                          s_model.screen_state && s_model.screen_state[0] ? s_model.screen_state : "检测中");
}

static void update_timer(void)
{
    uint32_t m = s_model.study_seconds / 60;
    uint32_t sec = s_model.study_seconds % 60;
    lv_label_set_text_fmt(s_timer_label, "%02" PRIu32 ":%02" PRIu32, m, sec);
}

static void update_alert(void)
{
    const char *title = s_event.type_text && s_event.type_text[0]
                            ? s_event.type_text
                            : ui_event_type_name(s_event.type);
    const char *msg = s_event.message && s_event.message[0]
                          ? s_event.message
                          : "我们先调整一下状态，再继续学习。";
    lv_label_set_text_fmt(s_alert_title, "%s  置信度 %u%%", title, s_event.confidence);
    lv_label_set_text(s_alert_msg, msg);
}

static void update_summary(void)
{
    const char *title = s_model.summary_title && s_model.summary_title[0]
                            ? s_model.summary_title
                            : (s_model.course_title && s_model.course_title[0] ? s_model.course_title : "今日课程");
    const char *text = s_model.summary_text && s_model.summary_text[0]
                           ? s_model.summary_text
                           : s_model.homework_text && s_model.homework_text[0]
                                 ? s_model.homework_text
                                 : "暂无总结";
    lv_label_set_text(s_summary_title, title);
    lv_label_set_text(s_summary_text, text);
}

static void set_face_expression(ui_overlay_mode_t mode);
static bool model_equal(const ui_model_t *a, const ui_model_t *b);
static bool event_equal(const ui_event_t *a, const ui_event_t *b);

static void set_visibility_by_mode(void)
{
    bool idle = (s_mode == UI_OVERLAY_IDLE);
    bool focus = (s_mode == UI_OVERLAY_FOCUS);
    bool alert = (s_mode == UI_OVERLAY_SOFT_ALERT);
    bool review = (s_mode == UI_OVERLAY_EVENT_REVIEW);
    bool summary = (s_mode == UI_OVERLAY_SUMMARY);

    lv_obj_set_style_opa(s_timer_label, (focus || alert) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_status_bar, (focus || alert || idle) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_alert_bar, (alert || review) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_summary_panel, summary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    /* AI face is the agent persona; hide it only when the summary takes over. */
    lv_obj_set_style_opa(s_face_container, summary ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

    set_face_expression(s_mode);
}

static void rebuild_ui_for_state(void)
{
    update_top_bar();
    update_status_row();
    update_timer();
    update_alert();
    update_summary();
    set_visibility_by_mode();
}

static void create_top_bar(lv_obj_t *parent)
{
    s_top_bar = lv_obj_create(parent);
    lv_obj_set_size(s_top_bar, s_ui_width, 40);
    lv_obj_set_pos(s_top_bar, 0, 0);
    set_container_style(s_top_bar, lv_color_hex(0x101010), LV_OPA_60);
    lv_obj_set_style_radius(s_top_bar, 0, 0);

    s_title_label = make_label(s_top_bar, "智学 Guardian", lv_color_white());
    lv_obj_set_pos(s_title_label, 10, 6);

    s_wifi_label = make_label(s_top_bar, "Wi-Fi:--", lv_color_white());
    lv_obj_set_pos(s_wifi_label, s_ui_width - 260, 6);

    s_cam_label = make_label(s_top_bar, "摄像头:--", lv_color_white());
    lv_obj_set_pos(s_cam_label, s_ui_width - 130, 6);
}

static void set_face_expression(ui_overlay_mode_t mode)
{
    if (!s_face_mouth) {
        return;
    }
    switch (mode) {
    case UI_OVERLAY_SOFT_ALERT:
    case UI_OVERLAY_EVENT_REVIEW:
        /* Surprised / worried: small round open mouth. */
        lv_obj_set_size(s_face_mouth, 16, 16);
        lv_obj_set_pos(s_face_mouth, 52, 80);
        lv_obj_set_style_radius(s_face_mouth, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_face_mouth, lv_color_hex(0xFF6B6B), 0);
        break;
    case UI_OVERLAY_FOCUS:
        /* Focused: tiny firm line. */
        lv_obj_set_size(s_face_mouth, 16, 4);
        lv_obj_set_pos(s_face_mouth, 52, 90);
        lv_obj_set_style_radius(s_face_mouth, 2, 0);
        lv_obj_set_style_bg_color(s_face_mouth, lv_color_white(), 0);
        break;
    case UI_OVERLAY_SUMMARY:
        /* Happy: wide smile. */
        lv_obj_set_size(s_face_mouth, 30, 10);
        lv_obj_set_pos(s_face_mouth, 45, 84);
        lv_obj_set_style_radius(s_face_mouth, 5, 0);
        lv_obj_set_style_bg_color(s_face_mouth, lv_color_hex(0x4ECDC4), 0);
        break;
    case UI_OVERLAY_IDLE:
    default:
        /* Idle: gentle smile. */
        lv_obj_set_size(s_face_mouth, 22, 7);
        lv_obj_set_pos(s_face_mouth, 49, 86);
        lv_obj_set_style_radius(s_face_mouth, 3, 0);
        lv_obj_set_style_bg_color(s_face_mouth, lv_color_white(), 0);
        break;
    }
}

static void create_face(lv_obj_t *parent)
{
    s_face_container = lv_obj_create(parent);
    lv_obj_set_size(s_face_container, 160, 190);
    lv_obj_set_pos(s_face_container, (s_ui_width - 160) / 2 - 5, 55);
    lv_obj_set_style_bg_opa(s_face_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_face_container, 0, 0);
    lv_obj_set_style_pad_all(s_face_container, 0, 0);

    /* Head: a glossy Kimi-blue sphere. */
    lv_obj_t *head = lv_obj_create(s_face_container);
    lv_obj_set_size(head, 120, 120);
    lv_obj_set_pos(head, 18, -2);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_bg_grad_color(head, lv_color_hex(0x1D4ED8), 0);
    lv_obj_set_style_bg_grad_dir(head, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_shadow_width(head, 18, 0);
    lv_obj_set_style_shadow_color(head, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(head, LV_OPA_30, 0);
    lv_obj_set_style_shadow_offset_x(head, 0, 0);
    lv_obj_set_style_shadow_offset_y(head, 5, 0);

    /* Gloss highlight, upper-left. */
    lv_obj_t *hl = lv_obj_create(head);
    lv_obj_set_size(hl, 44, 24);
    lv_obj_set_pos(hl, 18, 14);
    lv_obj_set_style_radius(hl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hl, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_20, 0);
    lv_obj_set_style_border_width(hl, 0, 0);

    /* Vertical oval eyes, symmetric around the head centre. */
    s_face_eye_l = lv_obj_create(head);
    lv_obj_set_size(s_face_eye_l, 18, 26);
    lv_obj_set_pos(s_face_eye_l, 29, 28);
    lv_obj_set_style_radius(s_face_eye_l, 13, 0);
    lv_obj_set_style_bg_color(s_face_eye_l, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_face_eye_l, 0, 0);

    s_face_eye_r = lv_obj_create(head);
    lv_obj_set_size(s_face_eye_r, 18, 26);
    lv_obj_set_pos(s_face_eye_r, 73, 28);
    lv_obj_set_style_radius(s_face_eye_r, 13, 0);
    lv_obj_set_style_bg_color(s_face_eye_r, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_face_eye_r, 0, 0);

    /* Tiny blue pupils, centred vertically in the eyes. */
    lv_obj_t *pupil_l = lv_obj_create(s_face_eye_l);
    lv_obj_set_size(pupil_l, 7, 11);
    lv_obj_center(pupil_l);
    lv_obj_set_style_radius(pupil_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil_l, lv_color_hex(0x1E3A8A), 0);
    lv_obj_set_style_border_width(pupil_l, 0, 0);

    lv_obj_t *pupil_r = lv_obj_create(s_face_eye_r);
    lv_obj_set_size(pupil_r, 7, 11);
    lv_obj_center(pupil_r);
    lv_obj_set_style_radius(pupil_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil_r, lv_color_hex(0x1E3A8A), 0);
    lv_obj_set_style_border_width(pupil_r, 0, 0);

    /* Mouth shape is set by set_face_expression(); just disable border here. */
    s_face_mouth = lv_obj_create(head);
    lv_obj_set_style_border_width(s_face_mouth, 0, 0);

    /* Subtle blush. */
    lv_obj_t *cheek_l = lv_obj_create(head);
    lv_obj_set_size(cheek_l, 12, 9);
    lv_obj_set_pos(cheek_l, 16, 74);
    lv_obj_set_style_radius(cheek_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cheek_l, lv_color_hex(0xF472B6), 0);
    lv_obj_set_style_bg_opa(cheek_l, LV_OPA_40, 0);
    lv_obj_set_style_border_width(cheek_l, 0, 0);

    lv_obj_t *cheek_r = lv_obj_create(head);
    lv_obj_set_size(cheek_r, 12, 9);
    lv_obj_set_pos(cheek_r, 92, 74);
    lv_obj_set_style_radius(cheek_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cheek_r, lv_color_hex(0xF472B6), 0);
    lv_obj_set_style_bg_opa(cheek_r, LV_OPA_40, 0);
    lv_obj_set_style_border_width(cheek_r, 0, 0);

    s_face_label = make_label(s_face_container, "AI 伴学", lv_color_white());
    lv_obj_set_style_text_font(s_face_label, ui_font(), 0);
    lv_obj_set_pos(s_face_label, 44, 128);
}

static void create_status_row(lv_obj_t *parent)
{
    s_status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_status_bar, s_ui_width - 120, 64);
    lv_obj_set_pos(s_status_bar, 100, s_height - 80);
    set_container_style(s_status_bar, lv_color_black(), LV_OPA_50);
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_seat_label = make_label(s_status_bar, "在座\n检测中", lv_color_white());
    lv_label_set_long_mode(s_seat_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_seat_label, LV_TEXT_ALIGN_CENTER, 0);

    s_pose_label = make_label(s_status_bar, "姿态\n检测中", lv_color_white());
    lv_label_set_long_mode(s_pose_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_pose_label, LV_TEXT_ALIGN_CENTER, 0);

    s_screen_label = make_label(s_status_bar, "页面\n检测中", lv_color_white());
    lv_label_set_long_mode(s_screen_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_screen_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void create_timer(lv_obj_t *parent)
{
    s_timer_label = lv_label_create(parent);
    lv_label_set_text(s_timer_label, "00:00");
    lv_obj_set_style_text_font(s_timer_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_timer_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_timer_label, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_timer_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_timer_label, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_timer_label, 8, 0);
    lv_obj_set_style_pad_all(s_timer_label, 12, 0);
    lv_obj_set_pos(s_timer_label, s_ui_width / 2 - 70, s_height / 2 - 30);
}

static void create_alert(lv_obj_t *parent)
{
    s_alert_bar = lv_obj_create(parent);
    lv_obj_set_size(s_alert_bar, s_ui_width - 40, 90);
    lv_obj_set_pos(s_alert_bar, 20, s_height - 120);
    set_container_style(s_alert_bar, lv_palette_main(LV_PALETTE_ORANGE), LV_OPA_80);

    s_alert_title = make_label(s_alert_bar, "提醒", lv_color_white());
    lv_obj_set_pos(s_alert_title, 10, 8);

    s_alert_msg = make_label(s_alert_bar, "", lv_color_white());
    lv_obj_set_pos(s_alert_msg, 10, 32);
    lv_obj_set_width(s_alert_msg, s_ui_width - 70);
    lv_label_set_long_mode(s_alert_msg, LV_LABEL_LONG_WRAP);
}

static void create_summary(lv_obj_t *parent)
{
    s_summary_panel = lv_obj_create(parent);
    lv_obj_set_size(s_summary_panel, s_ui_width - 80, s_height - 120);
    lv_obj_set_pos(s_summary_panel, 40, 70);
    set_container_style(s_summary_panel, lv_color_black(), LV_OPA_70);

    s_summary_title = make_label(s_summary_panel, "今日课程", lv_color_white());
    lv_obj_set_style_text_font(s_summary_title, ui_font(), 0);
    lv_obj_set_pos(s_summary_title, 20, 20);

    s_summary_text = make_label(s_summary_panel, "", lv_color_white());
    lv_obj_set_pos(s_summary_text, 20, 60);
    lv_obj_set_width(s_summary_text, s_ui_width - 140);
    lv_label_set_long_mode(s_summary_text, LV_LABEL_LONG_WRAP);
}

static void lvgl_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);

    /* Split the screen vertically: left half for the live camera feed,
     * right half for the UI overlay/dashboard. */
    s_cam_width = s_width / 2;
    s_ui_width = s_width - s_cam_width;

    /* UI panel occupies the right half. The camera side is copied directly
     * from s_cam_buf into s_display_buf by the LVGL task every frame, so
     * we do not draw it through LVGL (which was causing flicker). */
    lv_obj_t *ui_panel = lv_obj_create(scr);
    lv_obj_set_size(ui_panel, s_ui_width, s_height);
    lv_obj_set_pos(ui_panel, s_cam_width, 0);
    lv_obj_set_style_bg_color(ui_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ui_panel, 0, 0);
    lv_obj_set_style_pad_all(ui_panel, 0, 0);
    lv_obj_set_style_radius(ui_panel, 0, 0);

    create_top_bar(ui_panel);
    create_face(ui_panel);
    create_timer(ui_panel);
    create_status_row(ui_panel);
    create_alert(ui_panel);
    create_summary(ui_panel);

    rebuild_ui_for_state();
}

static void copy_camera_to_display(void)
{
    if (!s_cam_buf || !s_display_buf) {
        return;
    }

    size_t stride = (size_t)s_width * 2;
    size_t cam_row_bytes = (size_t)s_cam_width * 2;
    const uint8_t *src = (const uint8_t *)s_cam_buf;
    uint8_t *dst = (uint8_t *)s_display_buf;

    /* Make the ISP-written camera frame visible to the CPU. */
    esp_cache_msync(s_cam_buf, stride * s_height, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* Copy only the left half (the right half is owned by the UI overlay). */
    for (int y = 0; y < s_height; y++) {
        memcpy(dst + y * stride, src + y * stride, cam_row_bytes);
    }

    /* Flush the newly copied camera pixels so the DSI controller sees them. */
    esp_cache_msync(s_display_buf, stride * s_height, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void ui_lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL overlay task started");
    TickType_t last_tick = xTaskGetTickCount();

    while (true) {
        bool frame_ready = false;

        /* Wait for a new camera frame. */
        if (xSemaphoreTake(s_frame_ready_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
            frame_ready = true;
        }

        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = (now - last_tick) * portTICK_PERIOD_MS;
        last_tick = now;
        lv_tick_inc(elapsed_ms > 50 ? 50 : elapsed_ms);

        if (xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_model_updated) {
                s_model_updated = false;
                rebuild_ui_for_state();
            }
            xSemaphoreGive(s_state_lock);
        }

        /* Copy the live camera feed into the left half of the display buffer
         * directly; do not route it through LVGL to avoid full-screen redraws. */
        if (frame_ready) {
            copy_camera_to_display();
        }

        lv_timer_handler();
    }
}

void ui_lvgl_init(void *display_buf, void *cam_buf, int width, int height)
{
    s_display_buf = display_buf;
    s_cam_buf = cam_buf;
    s_width = width;
    s_height = height;

    s_state_lock = xSemaphoreCreateMutex();
    s_frame_ready_sem = xSemaphoreCreateBinary();
    s_update_sem = xSemaphoreCreateBinary();

    lv_init();

    s_disp = lv_display_create(width, height);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp,
                           display_buf,
                           NULL,
                           (size_t)width * height * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    lvgl_ui_create();

    xTaskCreate(ui_lvgl_task, "ui_lvgl", UI_LVGL_TASK_STACK_SIZE, NULL,
                UI_LVGL_TASK_PRIORITY, NULL);
}

void ui_lvgl_notify_frame_ready(BaseType_t *pxHigherPriorityTaskWoken)
{
    if (s_frame_ready_sem) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_frame_ready_sem, &woken);
        if (pxHigherPriorityTaskWoken && woken == pdTRUE) {
            *pxHigherPriorityTaskWoken = pdTRUE;
        }
    }
}

void ui_lvgl_update(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event)
{
    if (!model) {
        return;
    }

    /* LVGL overlay is initialized after the DSI/camera pipeline.  Ignore
     * early UI updates that arrive before the semaphore is created. */
    if (!s_state_lock) {
        return;
    }

    if (xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    bool event_empty = (event == NULL);
    bool current_event_empty = (s_event.type == UI_EVENT_NONE);
    bool unchanged = (mode == s_mode) &&
                     model_equal(model, &s_model) &&
                     ((event_empty && current_event_empty) ||
                      (!event_empty && event_equal(event, &s_event)));
    if (unchanged) {
        xSemaphoreGive(s_state_lock);
        return;
    }

    s_mode = mode;
    s_model = *model;
    if (event) {
        s_event = *event;
    } else {
        memset(&s_event, 0, sizeof(s_event));
        s_event.type = UI_EVENT_NONE;
    }
    s_model_updated = true;

    xSemaphoreGive(s_state_lock);
}

static bool string_equal(const char *a, const char *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static bool model_equal(const ui_model_t *a, const ui_model_t *b)
{
    if (!a || !b) {
        return false;
    }
    return a->wifi_connected == b->wifi_connected &&
           a->camera_ok == b->camera_ok &&
           a->privacy_on == b->privacy_on &&
           a->learn_score == b->learn_score &&
           a->risk_level == b->risk_level &&
           a->study_seconds == b->study_seconds &&
           a->event_duration_seconds == b->event_duration_seconds &&
           string_equal(a->seat_state, b->seat_state) &&
           string_equal(a->screen_state, b->screen_state) &&
           string_equal(a->pose_state, b->pose_state) &&
           string_equal(a->event_type, b->event_type) &&
           string_equal(a->event_message, b->event_message) &&
           string_equal(a->summary_title, b->summary_title) &&
           string_equal(a->summary_text, b->summary_text) &&
           string_equal(a->homework_text, b->homework_text) &&
           string_equal(a->course_title, b->course_title);
}

static bool event_equal(const ui_event_t *a, const ui_event_t *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return a->type == b->type &&
           a->confidence == b->confidence &&
           a->risk_level == b->risk_level &&
           a->duration_seconds == b->duration_seconds &&
           a->timestamp_seconds == b->timestamp_seconds &&
           a->bbox.valid == b->bbox.valid &&
           a->bbox.x == b->bbox.x &&
           a->bbox.y == b->bbox.y &&
           a->bbox.w == b->bbox.w &&
           a->bbox.h == b->bbox.h &&
           a->needs_review == b->needs_review &&
           string_equal(a->type_text, b->type_text) &&
           string_equal(a->message, b->message);
}

/* Strong symbol that overrides the weak stub in ui_pages.c. */
void ui_pages_draw_lvgl(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event)
{
    ui_lvgl_update(mode, model, event);
}
