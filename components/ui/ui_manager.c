#include "ui_manager.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ui_pages.h"

static const char *TAG = "guardian_ui";

#define UI_EVENT_MIN_CONFIDENCE 55
#define UI_EVENT_MIN_DURATION_SEC 3
#define UI_EVENT_COOLDOWN_SEC 30

typedef struct {
    ui_model_t model;
    char seat_state[32];
    char screen_state[32];
    char pose_state[32];
    char event_type[32];
    char event_message[128];
    char summary_title[64];
    char summary_text[360];
    char homework_text[120];
    char course_title[64];
} ui_state_storage_t;

typedef struct {
    ui_event_t event;
    char type_text[40];
    char message[160];
} ui_event_storage_t;

static SemaphoreHandle_t s_lock;
static ui_state_storage_t s_state;
static ui_event_storage_t s_event;
static ui_overlay_mode_t s_current_mode = UI_OVERLAY_IDLE;
static bool s_inited;
#if CONFIG_UI_MANAGER_DEMO_ENABLE
static bool s_demo_started;
#endif
static ui_event_type_t s_last_alert_type = UI_EVENT_NONE;
static TickType_t s_last_alert_tick;

static void copy_text(char *dst, size_t dst_size, const char *src, const char *fallback)
{
    if (dst_size == 0) {
        return;
    }

    const char *text = (src && src[0] != '\0') ? src : fallback;
    strlcpy(dst, text ? text : "", dst_size);
}

static const char *default_message_for_event(ui_event_type_t type)
{
    switch (type) {
    case UI_EVENT_LEAVE_SEAT:
        return "检测到你可能短暂离开座位，回来后我们继续学习就好。";
    case UI_EVENT_SCREEN_SWITCH:
        return "检测到课程页面可能切换了，我们先回到学习内容。";
    case UI_EVENT_PHONE_DISTRACTION:
        return "手机可能影响了学习节奏，先把注意力拉回这一小节。";
    case UI_EVENT_HEAD_DOWN:
        return "低头时间有点久，抬头放松一下会更舒服。";
    case UI_EVENT_LYING_DESK:
        return "检测到姿态可能偏低，调整坐姿后继续学习。";
    case UI_EVENT_HOMEWORK_STALL:
        return "作业进度暂时停住了，可以先从下一小问开始。";
    case UI_EVENT_MULTI_PERSON:
        return "画面里出现了多人，系统会优先关注当前学习者。";
    case UI_EVENT_CAMERA_BLOCKED:
        return "摄像头画面可能被遮挡，请确认镜头视野。";
    case UI_EVENT_PRIVACY_ON:
        return "隐私模式已开启，视觉识别会暂停。";
    default:
        return "我们先调整一下状态，再继续学习。";
    }
}

static void bind_model_strings(void)
{
    s_state.model.seat_state = s_state.seat_state;
    s_state.model.screen_state = s_state.screen_state;
    s_state.model.pose_state = s_state.pose_state;
    s_state.model.event_type = s_state.event_type;
    s_state.model.event_message = s_state.event_message;
    s_state.model.summary_title = s_state.summary_title;
    s_state.model.summary_text = s_state.summary_text;
    s_state.model.homework_text = s_state.homework_text;
    s_state.model.course_title = s_state.course_title;
}

static void bind_event_strings(void)
{
    s_event.event.type_text = s_event.type_text;
    s_event.event.message = s_event.message;
}

static void set_default_model(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.model.wifi_connected = false;
    s_state.model.camera_ok = false;
    s_state.model.privacy_on = false;
    s_state.model.learn_score = 86;
    s_state.model.risk_level = 1;
    s_state.model.study_seconds = 0;
    s_state.model.event_duration_seconds = 0;

    copy_text(s_state.seat_state, sizeof(s_state.seat_state), "检测中", "检测中");
    copy_text(s_state.screen_state, sizeof(s_state.screen_state), "等待课程开始", "等待课程开始");
    copy_text(s_state.pose_state, sizeof(s_state.pose_state), "检测中", "检测中");
    copy_text(s_state.event_type, sizeof(s_state.event_type), "学习状态提醒", "学习状态提醒");
    copy_text(s_state.event_message, sizeof(s_state.event_message), "准备好后就可以开始学习。", "准备好后就可以开始学习。");
    copy_text(s_state.summary_title, sizeof(s_state.summary_title), "今日课程", "今日课程");
    copy_text(s_state.summary_text, sizeof(s_state.summary_text),
              "1. 等待课程内容同步\n2. 准备进入专注守护",
              "1. 等待课程内容同步\n2. 准备进入专注守护");
    copy_text(s_state.homework_text, sizeof(s_state.homework_text), "暂无作业提醒", "暂无作业提醒");
    copy_text(s_state.course_title, sizeof(s_state.course_title), "智能伴学课程", "智能伴学课程");
    bind_model_strings();
}

static void set_empty_event(void)
{
    memset(&s_event, 0, sizeof(s_event));
    s_event.event.type = UI_EVENT_NONE;
    copy_text(s_event.type_text, sizeof(s_event.type_text), "无事件", "无事件");
    copy_text(s_event.message, sizeof(s_event.message), "保持当前学习节奏。", "保持当前学习节奏。");
    bind_event_strings();
}

static bool take_lock(void)
{
    if (!s_lock) {
        return true;
    }

    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE;
}

static void give_lock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void render_locked(void)
{
    ui_pages_render_overlay(s_current_mode, &s_state.model, &s_event.event);
}

static void copy_model(const ui_model_t *model)
{
    if (!model) {
        return;
    }

    s_state.model.wifi_connected = model->wifi_connected;
    s_state.model.camera_ok = model->camera_ok;
    s_state.model.privacy_on = model->privacy_on;
    s_state.model.learn_score = model->learn_score;
    s_state.model.risk_level = model->risk_level;
    s_state.model.study_seconds = model->study_seconds;
    s_state.model.event_duration_seconds = model->event_duration_seconds;

    copy_text(s_state.seat_state, sizeof(s_state.seat_state), model->seat_state, "检测中");
    copy_text(s_state.screen_state, sizeof(s_state.screen_state), model->screen_state, "检测中");
    copy_text(s_state.pose_state, sizeof(s_state.pose_state), model->pose_state, "检测中");
    copy_text(s_state.event_type, sizeof(s_state.event_type), model->event_type, "学习状态提醒");
    copy_text(s_state.event_message, sizeof(s_state.event_message), model->event_message, "我们先调整一下状态，再继续学习。");
    copy_text(s_state.summary_title, sizeof(s_state.summary_title), model->summary_title, "今日课程");
    copy_text(s_state.summary_text, sizeof(s_state.summary_text), model->summary_text,
              "1. 回顾本节课核心概念\n2. 标记仍需巩固的问题\n3. 完成课后练习");
    copy_text(s_state.homework_text, sizeof(s_state.homework_text), model->homework_text, "按老师要求完成课后任务");
    copy_text(s_state.course_title, sizeof(s_state.course_title), model->course_title, "智能伴学课程");
    bind_model_strings();
}

static bool text_changed(const char *a, const char *b)
{
    if (a == b) {
        return false;
    }
    if (!a || !b) {
        return true;
    }
    return strcmp(a, b) != 0;
}

static bool model_changed(const ui_model_t *a, const ui_model_t *b)
{
    return a->wifi_connected != b->wifi_connected ||
           a->camera_ok != b->camera_ok ||
           a->privacy_on != b->privacy_on ||
           a->learn_score != b->learn_score ||
           a->risk_level != b->risk_level ||
           a->study_seconds != b->study_seconds ||
           a->event_duration_seconds != b->event_duration_seconds ||
           text_changed(a->seat_state, b->seat_state) ||
           text_changed(a->screen_state, b->screen_state) ||
           text_changed(a->pose_state, b->pose_state) ||
           text_changed(a->event_type, b->event_type) ||
           text_changed(a->event_message, b->event_message) ||
           text_changed(a->summary_title, b->summary_title) ||
           text_changed(a->summary_text, b->summary_text) ||
           text_changed(a->homework_text, b->homework_text) ||
           text_changed(a->course_title, b->course_title);
}

static bool event_changed(const ui_event_t *a, const ui_event_t *b)
{
    if (!a || !b) {
        return true;
    }
    return a->type != b->type ||
           a->confidence != b->confidence ||
           a->risk_level != b->risk_level ||
           a->duration_seconds != b->duration_seconds ||
           a->timestamp_seconds != b->timestamp_seconds ||
           a->bbox.valid != b->bbox.valid ||
           a->bbox.x != b->bbox.x ||
           a->bbox.y != b->bbox.y ||
           a->bbox.w != b->bbox.w ||
           a->bbox.h != b->bbox.h ||
           a->needs_review != b->needs_review ||
           text_changed(a->type_text, b->type_text) ||
           text_changed(a->message, b->message);
}

static void copy_event(const ui_event_t *event)
{
    if (!event) {
        set_empty_event();
        return;
    }

    s_event.event = *event;
    copy_text(s_event.type_text, sizeof(s_event.type_text), event->type_text, ui_event_type_name(event->type));
    copy_text(s_event.message, sizeof(s_event.message), event->message, default_message_for_event(event->type));
    bind_event_strings();

    copy_text(s_state.event_type, sizeof(s_state.event_type), s_event.type_text, "学习状态提醒");
    copy_text(s_state.event_message, sizeof(s_state.event_message), s_event.message, "我们先调整一下状态，再继续学习。");
    s_state.model.event_duration_seconds = event->duration_seconds;
    if (event->risk_level > s_state.model.risk_level) {
        s_state.model.risk_level = event->risk_level;
    }
    bind_model_strings();
}

static bool should_surface_event(const ui_event_t *event)
{
    if (!event || event->type == UI_EVENT_NONE) {
        return false;
    }

    if (event->confidence > 0 && event->confidence < UI_EVENT_MIN_CONFIDENCE) {
        ESP_LOGI(TAG, "suppress event %s because confidence is %u%%", ui_event_type_name(event->type), event->confidence);
        return false;
    }

    if (event->duration_seconds < UI_EVENT_MIN_DURATION_SEC && event->risk_level < 2) {
        ESP_LOGI(TAG, "suppress event %s because duration is %" PRIu32 "s", ui_event_type_name(event->type), event->duration_seconds);
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t cooldown_ticks = pdMS_TO_TICKS(UI_EVENT_COOLDOWN_SEC * 1000);
    if (s_last_alert_type == event->type && (now - s_last_alert_tick) < cooldown_ticks && !event->needs_review) {
        ESP_LOGI(TAG, "suppress event %s because it is in cooldown", ui_event_type_name(event->type));
        return false;
    }

    s_last_alert_type = event->type;
    s_last_alert_tick = now;
    return true;
}

static ui_overlay_mode_t choose_overlay_for_event(const ui_event_t *event)
{
    if (!should_surface_event(event)) {
        return UI_OVERLAY_FOCUS;
    }

    if (event->bbox.valid || event->needs_review) {
        return UI_OVERLAY_EVENT_REVIEW;
    }

    return UI_OVERLAY_SOFT_ALERT;
}

#if CONFIG_UI_MANAGER_DEMO_ENABLE
static void ui_demo_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(3000));

    ui_model_t focus = {
        .wifi_connected = true,
        .camera_ok = true,
        .privacy_on = false,
        .learn_score = 88,
        .risk_level = 1,
        .seat_state = "在座",
        .screen_state = "课程页面",
        .pose_state = "坐姿正常",
        .event_type = "学习状态提醒",
        .event_message = "保持现在的节奏，继续完成这一小节。",
        .summary_title = "物联网导论",
        .summary_text = "1. 传感器负责采集环境信息\n2. ESP32-P4 适合视觉与显示交互\n3. 设备端 UI 只消费状态事件",
        .study_seconds = 125,
        .event_duration_seconds = 0,
        .homework_text = "整理本节课三个关键词",
        .course_title = "ESP32-P4 智能伴学课程",
    };
    ui_manager_update(&focus);
    ui_manager_show_page(UI_OVERLAY_FOCUS);

    vTaskDelay(pdMS_TO_TICKS(4000));
    ui_event_t leave_event = {
        .type = UI_EVENT_LEAVE_SEAT,
        .confidence = 78,
        .risk_level = 1,
        .duration_seconds = 12,
        .timestamp_seconds = 140,
    };
    ui_manager_push_event(&leave_event);

    vTaskDelay(pdMS_TO_TICKS(4000));
    ui_event_t phone_event = {
        .type = UI_EVENT_PHONE_DISTRACTION,
        .confidence = 86,
        .risk_level = 2,
        .duration_seconds = 8,
        .timestamp_seconds = 158,
        .bbox = {
            .valid = true,
            .x = 620,
            .y = 318,
            .w = 54,
            .h = 92,
        },
        .needs_review = true,
    };
    ui_manager_push_event(&phone_event);

    vTaskDelay(pdMS_TO_TICKS(4000));
    ui_event_t pose_event = {
        .type = UI_EVENT_HEAD_DOWN,
        .confidence = 72,
        .risk_level = 1,
        .duration_seconds = 18,
        .timestamp_seconds = 182,
    };
    ui_manager_push_event(&pose_event);

    vTaskDelay(pdMS_TO_TICKS(4000));

    ui_model_t summary = focus;
    summary.learn_score = 91;
    summary.risk_level = 0;
    summary.summary_title = "ESP32-P4 智能伴学课程";
    summary.summary_text = "1. 已完成摄像头与屏幕基础流程\n2. UI 模块通过状态模型接收事件\n3. 事件提醒采用温和表达\n4. 课程总结保留二维码区域";
    summary.homework_text = "完成项目展示稿中的 UI 交互说明";
    ui_manager_update(&summary);
    ui_manager_show_summary(summary.summary_title, summary.summary_text);

    ESP_LOGI(TAG, "UI overlay demo flow finished");
    vTaskDelete(NULL);
}
#endif

void ui_manager_init(void)
{
    if (s_inited) {
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    set_default_model();
    set_empty_event();
    s_current_mode = UI_OVERLAY_IDLE;
    s_inited = true;

    ESP_LOGI(TAG, "ui manager init done, mode=%s", ui_overlay_name(s_current_mode));
    render_locked();

#if CONFIG_UI_MANAGER_DEMO_ENABLE
    if (!s_demo_started) {
        BaseType_t ok = xTaskCreate(ui_demo_task, "guardian_ui_demo", 4096, NULL, 4, NULL);
        s_demo_started = (ok == pdPASS);
        if (!s_demo_started) {
            ESP_LOGW(TAG, "failed to start UI demo task");
        }
    }
#endif
}

void ui_manager_show_page(ui_page_t page)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip overlay change because UI lock is busy");
        return;
    }

    if (s_current_mode == page) {
        give_lock();
        return;
    }

    s_current_mode = page;
    render_locked();
    give_lock();
}

void ui_manager_update(const ui_model_t *model)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!model) {
        ESP_LOGW(TAG, "ignore empty UI model update");
        return;
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip model update because UI lock is busy");
        return;
    }

    if (!model_changed(model, &s_state.model)) {
        give_lock();
        return;
    }

    copy_model(model);
    render_locked();
    give_lock();
}

void ui_manager_push_event(const ui_event_t *event)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!event) {
        ui_manager_clear_event();
        return;
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip event because UI lock is busy");
        return;
    }

    if (!event_changed(event, &s_event.event)) {
        give_lock();
        return;
    }

    copy_event(event);
    s_current_mode = choose_overlay_for_event(&s_event.event);
    render_locked();
    give_lock();
}

void ui_manager_clear_event(void)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip clear event because UI lock is busy");
        return;
    }

    if (s_current_mode == UI_OVERLAY_FOCUS && s_event.event.type == UI_EVENT_NONE) {
        give_lock();
        return;
    }

    set_empty_event();
    s_current_mode = UI_OVERLAY_FOCUS;
    render_locked();
    give_lock();
}

void ui_manager_show_alert(const char *event_type, const char *message)
{
    ui_event_t event = {
        .type = UI_EVENT_CUSTOM,
        .type_text = event_type,
        .message = message,
        .confidence = 80,
        .risk_level = 1,
        .duration_seconds = 15,
        .timestamp_seconds = s_state.model.study_seconds,
    };
    ui_manager_push_event(&event);
}

void ui_manager_show_summary(const char *title, const char *summary)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip summary because UI lock is busy");
        return;
    }

    if (s_current_mode == UI_OVERLAY_SUMMARY &&
        !text_changed(title, s_state.summary_title) &&
        !text_changed(summary, s_state.summary_text)) {
        give_lock();
        return;
    }

    copy_text(s_state.summary_title, sizeof(s_state.summary_title), title, "今日课程");
    copy_text(s_state.summary_text, sizeof(s_state.summary_text), summary,
              "1. 回顾本节课核心概念\n2. 标记仍需巩固的问题\n3. 完成课后练习");
    bind_model_strings();
    set_empty_event();
    s_current_mode = UI_OVERLAY_SUMMARY;
    render_locked();
    give_lock();
}

void ui_manager_get_snapshot(ui_model_t *model, ui_event_t *event, ui_overlay_mode_t *mode)
{
    if (!s_inited) {
        ui_manager_init();
    }

    if (!take_lock()) {
        ESP_LOGW(TAG, "skip snapshot because UI lock is busy");
        return;
    }

    if (model) {
        *model = s_state.model;
    }
    if (event) {
        *event = s_event.event;
    }
    if (mode) {
        *mode = s_current_mode;
    }

    give_lock();
}
