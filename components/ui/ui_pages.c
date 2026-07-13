#include "ui_pages.h"

#include <inttypes.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "guardian_ui_pages";

static const char *safe_text(const char *text, const char *fallback)
{
    return (text && text[0] != '\0') ? text : fallback;
}

static const char *yes_no(bool enabled)
{
    return enabled ? "已连接" : "未连接";
}

static const char *on_off(bool enabled)
{
    return enabled ? "开启" : "关闭";
}

const char *ui_overlay_name(ui_overlay_mode_t mode)
{
    switch (mode) {
    case UI_OVERLAY_IDLE:
        return "待机叠层";
    case UI_OVERLAY_FOCUS:
        return "专注 HUD";
    case UI_OVERLAY_SOFT_ALERT:
        return "温和提醒叠层";
    case UI_OVERLAY_EVENT_REVIEW:
        return "事件框选叠层";
    case UI_OVERLAY_SUMMARY:
        return "课程总结叠层";
    default:
        return "未知叠层";
    }
}

const char *ui_page_name(ui_page_t page)
{
    return ui_overlay_name(page);
}

const char *ui_event_type_name(ui_event_type_t type)
{
    switch (type) {
    case UI_EVENT_NONE:
        return "无事件";
    case UI_EVENT_LEAVE_SEAT:
        return "疑似离座";
    case UI_EVENT_SCREEN_SWITCH:
        return "疑似切屏";
    case UI_EVENT_PHONE_DISTRACTION:
        return "手机干扰";
    case UI_EVENT_HEAD_DOWN:
        return "低头过久";
    case UI_EVENT_LYING_DESK:
        return "疑似趴桌";
    case UI_EVENT_HOMEWORK_STALL:
        return "作业停滞";
    case UI_EVENT_MULTI_PERSON:
        return "多人入镜";
    case UI_EVENT_CAMERA_BLOCKED:
        return "摄像头遮挡";
    case UI_EVENT_PRIVACY_ON:
        return "隐私模式";
    case UI_EVENT_CUSTOM:
        return "学习状态提醒";
    default:
        return "未知事件";
    }
}

/* LVGL/屏幕绘制入口预留给显示驱动接入后实现。 */
void __attribute__((weak)) ui_pages_draw_lvgl(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event)
{
    (void)mode;
    (void)model;
    (void)event;
    ESP_LOGD(TAG, "LVGL draw hook is not bound; overlay is logged only");
}

static const char *event_title(const ui_event_t *event)
{
    if (!event) {
        return "学习状态提醒";
    }

    return safe_text(event->type_text, ui_event_type_name(event->type));
}

static const char *event_message(const ui_event_t *event)
{
    if (!event) {
        return "保持当前学习节奏。";
    }

    return safe_text(event->message, "我们先调整一下状态，再继续学习。");
}

static void render_idle_overlay(const ui_model_t *model)
{
    ESP_LOGI(TAG,
             "video overlay idle | top-left: 智学 Guardian / 待机中 | top-right: Wi-Fi %s, 摄像头 %s | 隐私模式: %s | bottom hint: 轻触开始学习",
             yes_no(model->wifi_connected),
             model->camera_ok ? "正常" : "未就绪",
             on_off(model->privacy_on));
}

static void render_focus_overlay(const ui_model_t *model)
{
    uint32_t minutes = model->study_seconds / 60;
    uint32_t seconds = model->study_seconds % 60;

    ESP_LOGI(TAG,
             "video overlay focus | top-left: 专注守护中 %02" PRIu32 ":%02" PRIu32
             " | top-right: LearnScore %d, 风险 %d | bottom: 在座 %s / 页面 %s / 姿态 %s",
             minutes,
             seconds,
             model->learn_score,
             model->risk_level,
             safe_text(model->seat_state, "检测中"),
             safe_text(model->screen_state, "检测中"),
             safe_text(model->pose_state, "检测中"));
}

static void render_alert_overlay(const ui_model_t *model, const ui_event_t *event)
{
    ESP_LOGI(TAG,
             "video overlay alert | bottom subtitle: [%s] %s | duration %" PRIu32 "s | confidence %u%% | risk %d",
             event_title(event),
             event_message(event),
             event ? event->duration_seconds : model->event_duration_seconds,
             event ? event->confidence : 0,
             event ? event->risk_level : model->risk_level);
}

static void render_event_review_overlay(const ui_event_t *event)
{
    if (event && event->bbox.valid) {
        ESP_LOGI(TAG,
                 "video overlay review | event: %s | bbox x=%u y=%u w=%u h=%u | time %" PRIu32 "s",
                 event_title(event),
                 event->bbox.x,
                 event->bbox.y,
                 event->bbox.w,
                 event->bbox.h,
                 event->timestamp_seconds);
        return;
    }

    ESP_LOGI(TAG, "video overlay review | event: %s | bbox: waiting for algorithm", event_title(event));
}

static void render_summary_overlay(const ui_model_t *model)
{
    ESP_LOGI(TAG,
             "video overlay summary | dim video background | title: %s | summary: %s | homework: %s | QR reserved",
             safe_text(model->summary_title, safe_text(model->course_title, "今日课程")),
             safe_text(model->summary_text, "暂无总结"),
             safe_text(model->homework_text, "暂无作业提醒"));
}

void ui_pages_render_overlay(ui_overlay_mode_t mode, const ui_model_t *model, const ui_event_t *event)
{
    if (!model) {
        return;
    }

#if CONFIG_UI_MANAGER_USE_LVGL
    ui_pages_draw_lvgl(mode, model, event);
#endif

    ESP_LOGI(TAG, "render %s over live camera video", ui_overlay_name(mode));

    switch (mode) {
    case UI_OVERLAY_IDLE:
        render_idle_overlay(model);
        break;
    case UI_OVERLAY_FOCUS:
        render_focus_overlay(model);
        break;
    case UI_OVERLAY_SOFT_ALERT:
        render_alert_overlay(model, event);
        break;
    case UI_OVERLAY_EVENT_REVIEW:
        render_event_review_overlay(event);
        break;
    case UI_OVERLAY_SUMMARY:
        render_summary_overlay(model);
        break;
    default:
        ESP_LOGW(TAG, "unknown overlay mode: %d", mode);
        break;
    }
}

void ui_pages_render(ui_page_t page, const ui_model_t *model)
{
    ui_pages_render_overlay(page, model, NULL);
}
