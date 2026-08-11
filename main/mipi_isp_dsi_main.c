/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_awb.h"
#include "driver/isp_ccm.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "example_dsi_init.h"
#include "example_dsi_init_config.h"
#include "example_sensor_init.h"
#include "example_config.h"
#include "wifi_conn.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "audio.h"
#include "cloud_api.h"
#include "cloud_sync.h"
#include "pir.h"
#include "camera_capture.h"
#include "human_detect.h"
#include "reid_model.h"
#include "reid_gallery.h"
#include "ui_manager.h"
#include "ui_lvgl.h"
#if CONFIG_WAKE_WORD_ENABLED
#include "wake_word.h"
#endif

static const char *TAG = "mipi_isp_dsi";

static esp_cam_ctlr_handle_t s_cam_handle = NULL;
static int s_cam_width = 0;
static int s_cam_height = 0;
static void *s_frame_buffer = NULL;
static void *s_cam_buffer = NULL;
static void *s_display_buffer = NULL;
static size_t s_frame_buffer_size = 0;
static SemaphoreHandle_t s_cam_capture_mutex = NULL;
static SemaphoreHandle_t s_reid_mutex = NULL;
static ui_model_t s_ui_model;

/* Re-ID / focus session state */
static int s_current_person_id = -1;
static char s_current_identity_name[REID_MAX_NAME_LEN + 1] = {0};
static uint32_t s_study_start_ms = 0;
static time_t s_study_start_epoch = 0;
static bool s_study_active = false;
static bool s_enrolling = false;
static char s_enroll_name[REID_MAX_NAME_LEN + 1] = {0};
static int s_monitor_whitelist_id = -1;

/* Raw per-frame presence (no debounce) used to drive the identity label, and
 * a rate limiter so Re-ID inference does not run on every detection frame. */
static bool s_detected_person_now = false;

/* Cosine-similarity threshold for accepting a Re-ID match. Adjust this single
 * value to re-tune the match strictness. */
#define REID_MATCH_THRESHOLD 0.65f

/* Confidence (detection score) of the most recent confirmed person frame,
 * used as the "疑似离座" event confidence instead of a hard-coded value. */
static float s_last_detect_score = 0.0f;

/* Fallback leave-seat alert: fires when the frame has been continuously
 * "unrecognized" for ABSENT_ALERT_TIMEOUT_MS, regardless of whether debounced
 * presence (s_present) was ever asserted. This covers the case where the
 * person was never detected, so the present->absent edge never occurs and
 * on_human_left would never fire. */
static uint32_t s_absent_since_ms = 0;
static bool s_absent_alerted = false;
#define ABSENT_ALERT_TIMEOUT_MS 3000

/* -------------------------------------------------------------------------- */
/* TTS worker task                                                            */
/* -------------------------------------------------------------------------- */

#define TTS_WORKER_STACK_SIZE 32768
#define TTS_WORKER_QUEUE_LEN  4
#define TTS_TEXT_MAX_LEN      768

typedef struct {
    char text[TTS_TEXT_MAX_LEN];
} tts_job_t;

static QueueHandle_t s_tts_queue = NULL;
static StaticTask_t s_tts_worker_tcb;
static StackType_t *s_tts_worker_stack = NULL;

static void tts_worker_task(void *arg)
{
    (void)arg;
    tts_job_t job;

    while (1) {
        if (xQueueReceive(s_tts_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int16_t *pcm = NULL;
        size_t samples = 0;
        esp_err_t ret = cloud_tts_speak(job.text, &pcm, &samples);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TTS worker failed: %s", esp_err_to_name(ret));
            continue;
        }
        ESP_LOGI(TAG, "TTS worker playing %zu samples", samples);
        audio_play_pcm(pcm, samples);
        cloud_pcm_free(pcm);
    }
}

static void speak_text_enqueue(const char *text)
{
    if (s_tts_queue == NULL || text == NULL) {
        return;
    }

    tts_job_t job = {0};
    strlcpy(job.text, text, sizeof(job.text));
    if (xQueueSend(s_tts_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full, dropping: %s", text);
    }
}

static void tts_worker_start(void)
{
    if (s_tts_queue != NULL) {
        return;
    }

    s_tts_queue = xQueueCreate(TTS_WORKER_QUEUE_LEN, sizeof(tts_job_t));
    if (s_tts_queue == NULL) {
        ESP_LOGE(TAG, "failed to create TTS queue");
        return;
    }

    s_tts_worker_stack = (StackType_t *)heap_caps_malloc(TTS_WORKER_STACK_SIZE,
                                                         MALLOC_CAP_SPIRAM);
    if (s_tts_worker_stack == NULL) {
        ESP_LOGE(TAG, "failed to allocate TTS worker stack");
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        return;
    }

    TaskHandle_t handle = xTaskCreateStatic(tts_worker_task,
                                            "tts_worker",
                                            TTS_WORKER_STACK_SIZE / sizeof(StackType_t),
                                            NULL,
                                            5,
                                            s_tts_worker_stack,
                                            &s_tts_worker_tcb);
    if (handle == NULL) {
        ESP_LOGE(TAG, "failed to create TTS worker task");
        heap_caps_free(s_tts_worker_stack);
        s_tts_worker_stack = NULL;
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        return;
    }

    ESP_LOGI(TAG, "TTS worker task started");
}

/* -------------------------------------------------------------------------- */
/* Continuous monitoring state machine                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    MONITOR_STATE_IDLE,
    MONITOR_STATE_ACTIVE,
} monitor_state_t;

#define MONITOR_DEFAULT_DURATION_MS (5 * 60 * 1000)
#define MONITOR_PIR_COOLDOWN_MS     5000

static monitor_state_t s_monitor_state = MONITOR_STATE_IDLE;
static uint32_t s_monitor_deadline_ms = 0;
static uint32_t s_monitor_last_pir_ms = 0;
static uint8_t *s_monitor_ref_jpeg = NULL;
static size_t s_monitor_ref_jpeg_len = 0;
static SemaphoreHandle_t s_monitor_mutex = NULL;

static void monitor_init(void)
{
    if (s_monitor_mutex == NULL) {
        s_monitor_mutex = xSemaphoreCreateMutex();
    }
}

static bool monitor_is_active(void)
{
    bool active = false;
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    active = (s_monitor_state == MONITOR_STATE_ACTIVE);
    xSemaphoreGive(s_monitor_mutex);
    return active;
}

static bool extract_reid_feature(float *feature_out);

static bool monitor_is_command(const char *text, bool *start_out)
{
    if (text == NULL || start_out == NULL) {
        return false;
    }
    if (strstr(text, "开始监测") || strstr(text, "帮我看着") ||
        strstr(text, "监控一下") || strstr(text, "开始监控")) {
        *start_out = true;
        return true;
    }
    if (strstr(text, "停止监测") || strstr(text, "结束监测") ||
        strstr(text, "停止监控") || strstr(text, "结束监控")) {
        *start_out = false;
        return true;
    }
    return false;
}

static void monitor_clear_reference(void)
{
    if (s_monitor_ref_jpeg != NULL) {
        heap_caps_free(s_monitor_ref_jpeg);
        s_monitor_ref_jpeg = NULL;
        s_monitor_ref_jpeg_len = 0;
    }
}

static void monitor_set_reference(const uint8_t *jpeg, size_t len)
{
    monitor_clear_reference();
    if (jpeg == NULL || len == 0) {
        return;
    }
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate monitor reference frame");
        return;
    }
    memcpy(copy, jpeg, len);
    s_monitor_ref_jpeg = copy;
    s_monitor_ref_jpeg_len = len;
}

static void focus_stop(bool save);

static void monitor_start(void)
{
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    s_monitor_state = MONITOR_STATE_ACTIVE;
    s_monitor_deadline_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + MONITOR_DEFAULT_DURATION_MS;
    s_monitor_whitelist_id = -1;
    monitor_clear_reference();
    xSemaphoreGive(s_monitor_mutex);
    ESP_LOGI(TAG, "monitoring started");

    /* Capture the current identity as whitelist (do not create new entries). */
    if (human_is_present()) {
        float feature[REID_FEATURE_DIM];
        if (extract_reid_feature(feature)) {
            float sim = 0.0f;
            int id = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &sim);
            if (id > 0) {
                s_monitor_whitelist_id = id;
                ESP_LOGI(TAG, "monitor whitelist set to %s (id=%d, sim=%.4f)",
                         reid_gallery_get_name(id), id, sim);
            } else {
                ESP_LOGW(TAG, "monitor: could not identify current person for whitelist");
            }
        }
    }

    /* Stop any active focus session: the user is intentionally leaving the
     * seat for monitoring, so this must not be reported as a leave-seat event. */
    if (s_study_active) {
        focus_stop(true);
    }
}

static void monitor_stop(void)
{
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    s_monitor_state = MONITOR_STATE_IDLE;
    s_monitor_deadline_ms = 0;
    monitor_clear_reference();
    xSemaphoreGive(s_monitor_mutex);
    ESP_LOGI(TAG, "monitoring stopped");
}

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data);
static void http_test_request(void);

static void run_vlm_tts_pipeline(const char *question, const uint8_t *jpeg_data, size_t jpeg_len);
static bool question_needs_camera(const char *text);
static bool app_human_frame_reader(uint8_t *dst, size_t dst_len, int *width, int *height, void *ctx);
static void on_human_left(uint32_t duration_ms);
static void on_human_present(void);
static void on_human_left_reminder(uint32_t absent_ms);

static void focus_start(int person_id);
static void focus_stop(bool save);
static void update_focus_ui(void);
static bool parse_enroll_name(const char *text, char *name, size_t name_len);
static bool handle_voice_command(const char *text);
static void run_reid_and_update_ui(void);
static void focus_report(void);
static bool handle_clear_command(const char *text);
static void on_detect_frame(bool detected, const uint8_t *rgb565, int w, int h,
                            int x1, int y1, int x2, int y2, float score);

/* Return true if the user question likely requires visual information. */
static bool question_needs_camera(const char *text)
{
    if (text == NULL) {
        return false;
    }

    static const char *keywords[] = {
        "看见", "看到", "画面", "摄像头", "图片", "照片",
        "描述", "这里", "这", "那", "前面", "周围", "啥", "什么",
        NULL
    };

    for (int i = 0; keywords[i] != NULL; i++) {
        if (strstr(text, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void run_vlm_tts_pipeline(const char *question, const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (question == NULL) {
        return;
    }

    cloud_response_t vlm_result = {0};
    esp_err_t ret = cloud_vlm_ask(question, jpeg_data, jpeg_len, &vlm_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VLM failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "VLM result: %s", vlm_result.text ? vlm_result.text : "");

    speak_text_enqueue(vlm_result.text);
    cloud_response_free(&vlm_result);
}

static void speak_text(const char *text)
{
    if (text == NULL) {
        return;
    }
    speak_text_enqueue(text);
    ESP_LOGI(TAG, "TTS queued: %s", text);
}

static bool extract_reid_feature(float *feature_out)
{
    if (s_frame_buffer == NULL || s_cam_capture_mutex == NULL || s_reid_mutex == NULL ||
        s_cam_width <= 0 || s_cam_height <= 0 || feature_out == NULL) {
        return false;
    }

    xSemaphoreTake(s_reid_mutex, portMAX_DELAY);

    /* Default center crop for smoke-test / fallback. */
    int x1 = s_cam_width / 4;
    int y1 = s_cam_height / 8;
    int x2 = 3 * s_cam_width / 4;
    int y2 = 7 * s_cam_height / 8;

    int bx1, by1, bx2, by2;
    float score;
    if (human_detect_get_latest_bbox(&bx1, &by1, &bx2, &by2, &score)) {
        x1 = bx1;
        y1 = by1;
        x2 = bx2;
        y2 = by2;
        ESP_LOGI(TAG, "Re-ID using detect bbox (%d,%d,%d,%d) score=%.3f", x1, y1, x2, y2, score);
    } else {
        ESP_LOGI(TAG, "Re-ID fallback to center crop");
    }

    xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
    esp_cache_msync(s_frame_buffer, s_frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    bool ok = reid_model_extract((const uint8_t *)s_frame_buffer,
                                 s_cam_width, s_cam_height,
                                 x1, y1, x2, y2, feature_out);
    xSemaphoreGive(s_cam_capture_mutex);
    xSemaphoreGive(s_reid_mutex);
    return ok;
}

static void update_focus_ui(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t current_sec = 0;
    uint32_t total_sec = 0;

    if (s_study_active && s_current_person_id > 0) {
        current_sec = (now - s_study_start_ms) / 1000;
        const reid_person_t *p = reid_gallery_find_by_id(s_current_person_id);
        if (p) {
            total_sec = p->total_focus_seconds + current_sec;
        }
    } else if (s_current_person_id > 0) {
        const reid_person_t *p = reid_gallery_find_by_id(s_current_person_id);
        if (p) {
            total_sec = p->total_focus_seconds;
        }
    }

    /* The identity label follows the raw per-frame presence so it clears as
     * soon as the person leaves the frame, independent of the focus timer. */
    if (!s_detected_person_now) {
        s_ui_model.identity_name = "未识别";
    } else if (s_current_person_id > 0) {
        const reid_person_t *p = reid_gallery_find_by_id(s_current_person_id);
        if (p) {
            s_ui_model.identity_name = p->name;
        } else {
            s_ui_model.identity_name = s_current_identity_name[0] ? s_current_identity_name : "未知";
        }
    } else {
        s_ui_model.identity_name = s_current_identity_name[0] ? s_current_identity_name : "陌生人";
    }

    s_ui_model.current_focus_seconds = current_sec;
    s_ui_model.total_focus_seconds = total_sec;
    s_ui_model.study_seconds = current_sec;
}

static void focus_start(int person_id)
{
    if (person_id <= 0) {
        speak_text("未识别到注册身份，无法开始计时。");
        return;
    }
    if (s_study_active) {
        if (s_current_person_id == person_id) {
            speak_text("已经在学习计时中。");
            return;
        }
        focus_stop(true);
    }

    s_current_person_id = person_id;
    s_study_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_study_active = true;
    s_ui_model.screen_state = "正在学习中";
    strlcpy(s_current_identity_name, reid_gallery_get_name(person_id),
            sizeof(s_current_identity_name));
    s_study_start_epoch = time(NULL);
    cloud_sync_focus_start(s_current_identity_name, s_study_start_epoch);
    speak_text("开始学习，加油！");
    ESP_LOGI(TAG, "focus started for %s (id=%d)", s_current_identity_name, person_id);
    update_focus_ui();
}

static void focus_stop(bool save)
{
    if (!s_study_active) {
        s_study_active = false;
        s_current_person_id = -1;
        s_current_identity_name[0] = '\0';
        s_ui_model.screen_state = "等待课程开始";
        update_focus_ui();
        return;
    }

    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t duration_sec = (now - s_study_start_ms) / 1000;

    if (save && s_current_person_id > 0 && duration_sec > 0) {
        reid_gallery_add_focus_seconds(s_current_person_id, duration_sec);
    }

    if (duration_sec > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "本次学习结束，共 %02" PRIu32 ":%02" PRIu32 "。",
                 duration_sec / 60, duration_sec % 60);
        speak_text(msg);
    }

    if (s_current_person_id > 0 && duration_sec > 0) {
        cloud_sync_focus_end(s_current_identity_name, s_study_start_epoch,
                             time(NULL), duration_sec);
    }

    ESP_LOGI(TAG, "focus stopped for %s (id=%d), duration=%" PRIu32 "s, saved=%d",
             s_current_identity_name, s_current_person_id, duration_sec, save);

    s_study_active = false;
    s_current_person_id = -1;
    s_current_identity_name[0] = '\0';
    s_ui_model.screen_state = "等待课程开始";
    update_focus_ui();
}

static void focus_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* Fallback leave-seat alert: if the frame has been "unrecognized" for a
         * while, surface the alert even when debounced presence was never
         * asserted (so on_human_left never fires, e.g. person never detected). */
        if (!s_detected_person_now && !monitor_is_active()) {
            if (s_absent_since_ms == 0) {
                s_absent_since_ms = now;
            } else if (!s_absent_alerted &&
                       (now - s_absent_since_ms) >= ABSENT_ALERT_TIMEOUT_MS) {
                s_absent_alerted = true;
                int conf = (int)(s_last_detect_score * 100.0f + 0.5f);
                if (conf < 60) {
                    conf = 60;  /* keep above UI_EVENT_MIN_CONFIDENCE (55) */
                } else if (conf > 100) {
                    conf = 100;
                }
                ui_event_t leave_event = {
                    .type = UI_EVENT_LEAVE_SEAT,
                    .confidence = conf,
                    .risk_level = 1,
                    .duration_seconds = (now - s_absent_since_ms) / 1000,
                    .timestamp_seconds = (now - s_absent_since_ms) / 1000,
                };
                ui_manager_push_event(&leave_event);
                cloud_sync_alert(CLOUD_SYNC_ALERT_LEAVE_SEAT, conf,
                                 "持续未识别，疑似离座");
                ESP_LOGI(TAG, "absent alert fired after %" PRIu32 " ms unrecognized",
                         now - s_absent_since_ms);
            }
        } else {
            s_absent_since_ms = 0;
            s_absent_alerted = false;
        }

        if (!human_is_present()) {
            if (s_current_person_id > 0) {
                s_current_person_id = -1;
                s_current_identity_name[0] = '\0';
                update_focus_ui();
                ui_manager_update(&s_ui_model);
                ESP_LOGI(TAG, "focus tick: no person detected, cleared cached identity");
            }
        }

        if (s_study_active) {
            update_focus_ui();
            ui_manager_update_timer(s_ui_model.current_focus_seconds,
                                    s_ui_model.total_focus_seconds);
        }
    }
}

static bool is_utf8_comma(const char *p)
{
    return ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC && (unsigned char)p[2] == 0x8C);
}

static bool is_utf8_period(const char *p)
{
    return ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x82);
}

static bool parse_enroll_name(const char *text, char *name, size_t name_len)
{
    if (text == NULL || name == NULL || name_len == 0) {
        return false;
    }
    const char *prefix = strstr(text, "我叫");
    if (!prefix) {
        prefix = strstr(text, "我是");
    }
    if (!prefix) {
        return false;
    }
    const char *p = prefix + (prefix == strstr(text, "我叫") ? strlen("我叫") : strlen("我是"));
    while (*p != '\0') {
        if (*p == ' ' || *p == ',') {
            p++;
        } else if (is_utf8_comma(p)) {
            p += 3;
        } else {
            break;
        }
    }
    if (*p == '\0') {
        return false;
    }
    size_t len = 0;
    while (*p != '\0' && len + 1 < name_len) {
        if (*p == ' ' || *p == ',' || *p == '.' || *p == '\n' || *p == '\r' ||
            is_utf8_comma(p) || is_utf8_period(p)) {
            break;
        }
        name[len++] = *p++;
    }
    name[len] = '\0';
    return len > 0;
}

static bool is_focus_query_command(const char *text)
{
    if (text == NULL) {
        return false;
    }
    static const char *keywords[] = {
        "学了多久", "学习多久", "专注时长", "专注多久",
        "学习记录", "专注记录", "我的记录", "累计时长",
        "累计学习", "我的专注", "我的学习", NULL,
    };
    for (int i = 0; keywords[i] != NULL; ++i) {
        if (strstr(text, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void focus_report(void)
{
    if (!human_is_present()) {
        if (s_current_person_id > 0) {
            s_current_person_id = -1;
            s_current_identity_name[0] = '\0';
            update_focus_ui();
            ui_manager_update(&s_ui_model);
            ESP_LOGI(TAG, "focus report: no person detected, cleared cached identity");
        }
        speak_text("未检测到有人，请站到摄像头前。");
        return;
    }

    /* Person is present. Re-identify from the current frame to avoid
     * reporting stale cached identity. */
    float feature[REID_FEATURE_DIM];
    if (!extract_reid_feature(feature)) {
        speak_text("识别失败，请调整位置。");
        return;
    }

    float sim = 0.0f;
    int id = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &sim);
    if (id > 0) {
        s_current_person_id = id;
        strlcpy(s_current_identity_name, reid_gallery_get_name(id),
                sizeof(s_current_identity_name));
        ESP_LOGI(TAG, "focus report: identified %s (id=%d), sim=%.4f",
                 s_current_identity_name, id, sim);
    } else {
        s_current_person_id = -1;
        s_current_identity_name[0] = '\0';
        update_focus_ui();
        ui_manager_update(&s_ui_model);
        speak_text("未识别到注册身份。");
        ESP_LOGI(TAG, "focus report: no registered identity, sim=%.4f", sim);
        return;
    }

    const reid_person_t *p = reid_gallery_find_by_id(s_current_person_id);
    if (p == NULL) {
        speak_text("未找到你的学习记录。");
        return;
    }

    uint32_t current_sec = 0;
    if (s_study_active && s_current_person_id == p->person_id) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        current_sec = (now - s_study_start_ms) / 1000;
    }

    uint32_t total_sec = p->total_focus_seconds + current_sec;

    char msg[128];
    if (current_sec > 0 && total_sec > current_sec) {
        snprintf(msg, sizeof(msg),
                 "你当前专注 %02" PRIu32 ":%02" PRIu32 "，累计专注 %02" PRIu32 ":%02" PRIu32 "。",
                 current_sec / 60, current_sec % 60,
                 total_sec / 60, total_sec % 60);
    } else if (total_sec > 0) {
        snprintf(msg, sizeof(msg),
                 "你累计专注 %02" PRIu32 ":%02" PRIu32 "。",
                 total_sec / 60, total_sec % 60);
    } else {
        snprintf(msg, sizeof(msg), "还没有学习记录。");
    }
    speak_text(msg);
    ESP_LOGI(TAG, "focus report: %s total=%" PRIu32 "s current=%" PRIu32 "s",
             p->name, total_sec, current_sec);
}

static bool parse_clear_command(const char *text, bool *clear_all, bool *reset_focus, char *name, size_t name_len)
{
    if (text == NULL || clear_all == NULL || reset_focus == NULL || name == NULL || name_len == 0) {
        return false;
    }

    *clear_all = false;
    *reset_focus = false;
    name[0] = '\0';

    bool has_all = strstr(text, "全部") || strstr(text, "所有") || strstr(text, "所有记录");
    bool has_clear_action = strstr(text, "清除") || strstr(text, "删除") ||
                            strstr(text, "清空") || strstr(text, "清零") || strstr(text, "去掉");
    if (!has_clear_action) {
        return false;
    }

    if (has_all) {
        *clear_all = true;
        return true;
    }

    bool has_focus_time = strstr(text, "专注时长") || strstr(text, "学习时长") ||
                          strstr(text, "累计") || strstr(text, "清零");
    bool has_record = strstr(text, "记录") || strstr(text, "数据");
    if (has_focus_time) {
        *reset_focus = true;
    } else if (has_record) {
        *reset_focus = false;
    } else if (strstr(text, "清空") || strstr(text, "清零")) {
        *reset_focus = true;
    } else {
        *reset_focus = false;
    }

    const char *prefix = NULL;
    const char *prefix_end = NULL;
    const char *action_prefixes[] = {"删除", "清除", "去掉", "清空", "清零", NULL};
    for (int i = 0; action_prefixes[i] != NULL; i++) {
        const char *p = strstr(text, action_prefixes[i]);
        if (p != NULL && (prefix == NULL || p < prefix)) {
            prefix = p;
            prefix_end = p + strlen(action_prefixes[i]);
        }
    }
    if (prefix == NULL) {
        return false;
    }

    const char *p = prefix_end;
    const char *end = text + strlen(text);
    const char *suffix = strstr(p, "的");
    if (suffix != NULL) {
        end = suffix;
    }

    if (strncmp(p, "我的", strlen("我的")) == 0) {
        p += strlen("我的");
    } else if (strncmp(p, "我", strlen("我")) == 0) {
        p += strlen("我");
    }

    while (p < end && (*p == ' ' || *p == ',' || *p == '.')) {
        p++;
    }
    while (end > p && (*(end - 1) == ' ' || *(end - 1) == ',' || *(end - 1) == '.')) {
        end--;
    }

    size_t len = 0;
    while (p < end && len + 1 < name_len) {
        name[len++] = *p++;
    }
    name[len] = '\0';

    return true;
}

static bool handle_clear_command(const char *text)
{
    bool clear_all = false;
    bool reset_focus = false;
    char name[REID_MAX_NAME_LEN + 1];

    if (!parse_clear_command(text, &clear_all, &reset_focus, name, sizeof(name))) {
        return false;
    }

    if (clear_all) {
        reid_gallery_reset();
        s_current_person_id = -1;
        s_current_identity_name[0] = '\0';
        s_enrolling = false;
        s_enroll_name[0] = '\0';
        update_focus_ui();
        speak_text("已清除所有记录。");
        ESP_LOGI(TAG, "gallery cleared by voice command");
        ui_manager_update(&s_ui_model);
        return true;
    }

    int target_id = -1;
    if (name[0] != '\0') {
        target_id = reid_gallery_find_by_name(name);
        if (target_id < 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "未找到 %s 的记录。", name);
            speak_text(msg);
            return true;
        }
    } else if (s_current_person_id > 0) {
        target_id = s_current_person_id;
    } else {
        float feature[REID_FEATURE_DIM];
        if (extract_reid_feature(feature)) {
            float sim = 0.0f;
            target_id = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &sim);
            if (target_id > 0) {
                s_current_person_id = target_id;
                strlcpy(s_current_identity_name, reid_gallery_get_name(target_id),
                        sizeof(s_current_identity_name));
                ESP_LOGI(TAG, "clear command: identified %s (id=%d), sim=%.4f",
                         s_current_identity_name, target_id, sim);
            }
        }
    }

    if (target_id <= 0) {
        speak_text("未识别到目标，请站到摄像头前再说。");
        return true;
    }

    const char *target_name = reid_gallery_get_name(target_id);
    char msg[64];
    if (reset_focus) {
        reid_gallery_reset_focus_time(target_id);
        if (target_id == s_current_person_id) {
            update_focus_ui();
        }
        snprintf(msg, sizeof(msg), "已清空 %s 的专注时长。", target_name);
        ESP_LOGI(TAG, "cleared focus time for %s (id=%d)", target_name, target_id);
    } else {
        reid_gallery_delete_person(target_id);
        if (target_id == s_current_person_id) {
            s_current_person_id = -1;
            s_current_identity_name[0] = '\0';
            update_focus_ui();
        }
        snprintf(msg, sizeof(msg), "已删除 %s 的记录。", target_name);
        ESP_LOGI(TAG, "deleted person %s (id=%d)", target_name, target_id);
    }
    speak_text(msg);
    ui_manager_update(&s_ui_model);
    return true;
}

static bool handle_voice_command(const char *text)
{
    if (text == NULL) {
        return false;
    }

    if (handle_clear_command(text)) {
        ui_manager_update(&s_ui_model);
        return true;
    }

    char name[REID_MAX_NAME_LEN + 1];
    if (parse_enroll_name(text, name, sizeof(name))) {
        ESP_LOGI(TAG, "voice enroll command, name=%s", name);
        if (!human_is_present()) {
            speak_text("请站到摄像头前，再对我说记住你。");
            return true;
        }
        if (reid_gallery_enroll_start(name)) {
            strlcpy(s_enroll_name, name, sizeof(s_enroll_name));
            s_enrolling = true;
            speak_text("请保持面向摄像头，正在采集。");
            run_reid_and_update_ui();
        } else {
            speak_text("注册失败，Gallery 已满。");
        }
        return true;
    }

    if (strstr(text, "开始学习") || strstr(text, "开始计时") || strstr(text, "我要学习")) {
        if (s_current_person_id > 0) {
            focus_start(s_current_person_id);
        } else {
            speak_text("请先让我知道你是谁，或者注册后再开始学习。");
        }
        return true;
    }

    if (strstr(text, "结束学习") || strstr(text, "停止计时") || strstr(text, "不学了")) {
        focus_stop(true);
        return true;
    }

    if (is_focus_query_command(text)) {
        focus_report();
        return true;
    }

    return false;
}

/* Run Re-ID inference on an explicit RGB565 frame + bbox. Holds s_reid_mutex. */
static bool reid_extract_from_frame(const uint8_t *rgb565, int w, int h,
                                    int x1, int y1, int x2, int y2, float *feature_out)
{
    if (rgb565 == NULL || feature_out == NULL || s_reid_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_reid_mutex, portMAX_DELAY);
    bool ok = reid_model_extract(rgb565, w, h, x1, y1, x2, y2, feature_out);
    xSemaphoreGive(s_reid_mutex);
    return ok;
}

/* Match a feature against the gallery (or feed an ongoing enrollment) and
 * update s_current_person_id / s_current_identity_name accordingly. */
static void reid_match_and_set_identity(const float *feature, uint32_t now_ms)
{
    if (s_enrolling) {
        if (reid_gallery_enroll_feed(feature)) {
            int id = reid_gallery_enroll_finish();
            s_enrolling = false;
            if (id > 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "我记住你了，%s。", reid_gallery_get_name(id));
                speak_text(msg);
                s_current_person_id = id;
                strlcpy(s_current_identity_name, reid_gallery_get_name(id),
                        sizeof(s_current_identity_name));
            } else {
                speak_text("注册失败，请重试。");
            }
        }
        return;
    }

    float match_sim = 0.0f;
    int person_id = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &match_sim);
    if (person_id > 0) {
        /* Re-run update so that the matched template gets EMA-updated. */
        reid_gallery_update(feature, now_ms, &match_sim);
        s_current_person_id = person_id;
        strlcpy(s_current_identity_name, reid_gallery_get_name(person_id),
                sizeof(s_current_identity_name));
        ESP_LOGI(TAG, "Re-ID matched %s (id=%d), sim=%.4f",
                 s_current_identity_name, person_id, match_sim);
    } else {
        /* Unknown person detected but do not auto-register a new gallery entry;
         * this prevents the gallery from filling with transient strangers. */
        s_current_person_id = 0;
        strlcpy(s_current_identity_name, "陌生人", sizeof(s_current_identity_name));
        ESP_LOGI(TAG, "Re-ID unknown person, sim=%.4f", match_sim);
    }
}

static void run_reid_and_update_ui(void)
{
    float feature[REID_FEATURE_DIM];
    if (!extract_reid_feature(feature)) {
        ESP_LOGW(TAG, "Re-ID feature extraction failed");
        return;
    }
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_detected_person_now = true;
    reid_match_and_set_identity(feature, now_ms);
    update_focus_ui();
}

/* Per-frame human detection callback: re-identify on every confirmed person
 * frame (rate-limited) and clear the identity label as soon as a frame no
 * longer contains a person, without waiting for the leave-timeout debounce. */
static void on_detect_frame(bool detected, const uint8_t *rgb565, int w, int h,
                            int x1, int y1, int x2, int y2, float score)
{
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!detected) {
        /* No person this frame: show 未识别, following the detect result. */
        if (s_detected_person_now || s_current_person_id != -1) {
            s_detected_person_now = false;
            s_current_person_id = -1;
            s_current_identity_name[0] = '\0';
            update_focus_ui();
            ui_manager_update(&s_ui_model);
        }
        return;
    }

    s_last_detect_score = score;

    if (s_enrolling) {
        s_detected_person_now = true;
        /* Feed every frame while enrolling to collect more samples. */
        float feature[REID_FEATURE_DIM];
        if (reid_extract_from_frame(rgb565, w, h, x1, y1, x2, y2, feature)) {
            reid_match_and_set_identity(feature, now_ms);
            update_focus_ui();
            ui_manager_update(&s_ui_model);
        }
        return;
    }

    /* detected=true: run Re-ID and update the UI identity on every frame. */
    float feature[REID_FEATURE_DIM];
    if (!reid_extract_from_frame(rgb565, w, h, x1, y1, x2, y2, feature)) {
        return;
    }

    float sim = 0.0f;
    int qid = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &sim);

    if (qid > 0) {
        /* Matched a registered identity: update identity + template every frame. */
        s_detected_person_now = true;
        reid_match_and_set_identity(feature, now_ms);
    } else {
        /* Detected a shape but no registered match: show 未识别. */
        s_detected_person_now = false;
        s_current_person_id = -1;
        s_current_identity_name[0] = '\0';
    }
    update_focus_ui();
    ui_manager_update(&s_ui_model);  /* follow the detect result every frame */
}

static bool app_human_frame_reader(uint8_t *dst, size_t dst_len, int *width, int *height, void *ctx)
{
    (void)ctx;

    if (dst == NULL || width == NULL || height == NULL ||
        s_frame_buffer == NULL || s_cam_capture_mutex == NULL ||
        s_cam_width <= 0 || s_cam_height <= 0) {
        return false;
    }

    size_t frame_len = (size_t)s_cam_width * (size_t)s_cam_height * EXAMPLE_RGB565_BITS_PER_PIXEL / 8;
    if (dst_len < frame_len || s_frame_buffer_size < frame_len) {
        ESP_LOGW(TAG, "human frame reader buffer too small: dst=%zu frame=%zu live=%zu",
                 dst_len, frame_len, s_frame_buffer_size);
        return false;
    }

    xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
    esp_cache_msync(s_frame_buffer, frame_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(dst, s_frame_buffer, frame_len);
    xSemaphoreGive(s_cam_capture_mutex);

    *width = s_cam_width;
    *height = s_cam_height;
    return true;
}

static void on_human_left(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "human left callback, presence_duration=%" PRIu32 " ms", duration_ms);

    focus_stop(true);
    s_current_person_id = -1;
    s_current_identity_name[0] = '\0';
    s_ui_model.seat_state = "离座";
    s_ui_model.event_duration_seconds = duration_ms / 1000;
    update_focus_ui();
    ui_manager_update(&s_ui_model);

    int leave_conf = (int)(s_last_detect_score * 100.0f + 0.5f);
    if (leave_conf <= 0) {
        leave_conf = 50;  /* fallback when no detection score was recorded */
    } else if (leave_conf > 100) {
        leave_conf = 100;
    }
    ui_event_t leave_event = {
        .type = UI_EVENT_LEAVE_SEAT,
        .confidence = leave_conf,
        .risk_level = 1,
        .duration_seconds = duration_ms / 1000,
        .timestamp_seconds = duration_ms / 1000,
    };
    ui_manager_push_event(&leave_event);
}

static void on_human_present(void)
{
    ESP_LOGI(TAG, "human present callback");

    s_ui_model.seat_state = "在座";
    run_reid_and_update_ui();
    ui_manager_update(&s_ui_model);

    /* Roll back from a soft-alert / review state to the normal focus overlay. */
    ui_manager_clear_event();
}

static void on_human_left_reminder(uint32_t absent_ms)
{
    if (monitor_is_active()) {
        /* During monitoring the user is intentionally away from the seat. */
        ESP_LOGI(TAG, "monitoring active, suppress leave-seat reminder");
        return;
    }
    ESP_LOGI(TAG, "human left reminder, absent_ms=%" PRIu32, absent_ms);
    int conf = (int)(s_last_detect_score * 100.0f + 0.5f);
    cloud_sync_alert(CLOUD_SYNC_ALERT_LEAVE_SEAT, conf,
                     "离开座位超过一分钟");
    speak_text("你已经离开座位一分钟了，回来我们继续学习吧。");
}
#if CONFIG_WAKE_WORD_ENABLED
static void on_wake_word_detected(void)
{
    ESP_LOGI(TAG, "wake word detected callback");

    /* Record 3 seconds of audio after wake word. */
    int16_t *rec_pcm = NULL;
    size_t rec_samples = 0;
    esp_err_t ret = audio_record(3000, &rec_pcm, &rec_samples);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio record failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "recorded %zu samples for ASR", rec_samples);

    /* ASR: speech to text. */
    cloud_response_t asr_result = {0};
    ret = cloud_asr_transcribe(rec_pcm, rec_samples, &asr_result);
    audio_free_pcm(rec_pcm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ASR failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "ASR result: %s", asr_result.text);
    if (asr_result.text == NULL || asr_result.text[0] == '\0') {
        ESP_LOGW(TAG, "ASR returned empty text, skipping VLM/TTS");
        cloud_response_free(&asr_result);
        return;
    }

    /* Handle Re-ID enrollment and focus-timer voice commands first. */
    if (handle_voice_command(asr_result.text)) {
        ui_manager_update(&s_ui_model);
        cloud_response_free(&asr_result);
        return;
    }

    /* Handle monitoring start/stop commands. */
    bool start_monitor = false;
    if (monitor_is_command(asr_result.text, &start_monitor)) {
        if (start_monitor) {
            monitor_start();
            /* Capture the first frame as the reference. */
            uint8_t *ref_jpeg = NULL;
            size_t ref_len = 0;
            if (s_frame_buffer != NULL && s_cam_capture_mutex != NULL) {
                xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
                esp_err_t cap_ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                                        s_cam_width, s_cam_height,
                                                        &ref_jpeg, &ref_len);
                xSemaphoreGive(s_cam_capture_mutex);
                if (cap_ret == ESP_OK) {
                    monitor_set_reference(ref_jpeg, ref_len);
                    heap_caps_free(ref_jpeg);
                }
            }
            speak_text("已开始监测环境，我会把检测到的变化语音告诉你。");
        } else {
            monitor_stop();
            speak_text("监测已结束。");
        }
        cloud_response_free(&asr_result);
        return;
    }

    /* Capture a frame only when the question likely needs visual context. */
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    bool need_camera = question_needs_camera(asr_result.text);
    ESP_LOGI(TAG, "question needs camera: %s", need_camera ? "yes" : "no");
    if (need_camera && s_frame_buffer != NULL && s_cam_capture_mutex != NULL) {
        xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
        esp_err_t cap_ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                                s_cam_width, s_cam_height,
                                                &jpeg_buf, &jpeg_len);
        xSemaphoreGive(s_cam_capture_mutex);
        if (cap_ret != ESP_OK) {
            ESP_LOGW(TAG, "failed to capture frame for VLM");
        }
    }

    /* VLM + TTS pipeline. */
    run_vlm_tts_pipeline(asr_result.text, jpeg_buf, jpeg_len);
    cloud_response_free(&asr_result);
    if (jpeg_buf != NULL) {
        heap_caps_free(jpeg_buf);
    }

    ESP_LOGI(TAG, "interaction pipeline done");
}
#endif

#if CONFIG_PIR_SENSOR_ENABLED

/* Return true if the VLM response indicates no meaningful change. */
static bool monitor_response_is_silent(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return true;
    }
    if (strstr(text, "NO_CHANGE") != NULL) {
        return true;
    }
    if (strstr(text, "没有变化") != NULL || strstr(text, "无变化") != NULL ||
        strstr(text, "没有明显变化") != NULL || strstr(text, "无明显变化") != NULL) {
        return true;
    }
    return false;
}

static void on_pir_motion_detected(void)
{
    ESP_LOGI(TAG, "PIR motion detected callback");

    if (s_frame_buffer == NULL) {
        ESP_LOGW(TAG, "camera not ready, ignoring PIR event");
        return;
    }

    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);

    /* Check monitoring state and timeout. */
    if (s_monitor_state != MONITOR_STATE_ACTIVE) {
        xSemaphoreGive(s_monitor_mutex);
        return;
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now >= s_monitor_deadline_ms) {
        s_monitor_state = MONITOR_STATE_IDLE;
        monitor_clear_reference();
        xSemaphoreGive(s_monitor_mutex);
        run_vlm_tts_pipeline("监测已自动结束。", NULL, 0);
        return;
    }

    if (now - s_monitor_last_pir_ms < MONITOR_PIR_COOLDOWN_MS) {
        ESP_LOGI(TAG, "PIR event within monitoring cooldown, skip");
        xSemaphoreGive(s_monitor_mutex);
        return;
    }
    s_monitor_last_pir_ms = now;

    xSemaphoreGive(s_monitor_mutex);

    /* Capture current frame. */
    uint8_t *cur_jpeg = NULL;
    size_t cur_len = 0;
    xSemaphoreTake(s_cam_capture_mutex, portMAX_DELAY);
    esp_err_t ret = camera_capture_jpeg((const uint8_t *)s_frame_buffer,
                                        s_cam_width, s_cam_height,
                                        &cur_jpeg, &cur_len);
    xSemaphoreGive(s_cam_capture_mutex);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor keyframe capture failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Re-ID whitelist check: if the moving person is the registered user,
     * do not bother the VLM/TTS path. */
    if (s_monitor_whitelist_id > 0 && human_is_present()) {
        float feature[REID_FEATURE_DIM];
        if (extract_reid_feature(feature)) {
            float sim = 0.0f;
            int id = reid_gallery_query(feature, REID_MATCH_THRESHOLD, &sim);
            if (id == s_monitor_whitelist_id) {
                ESP_LOGI(TAG, "monitor: whitelist identity %s detected, skip VLM",
                         reid_gallery_get_name(id));
                heap_caps_free(cur_jpeg);
                return;
            }
        }
    }

    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);

    /* First trigger: save reference frame and stay silent. */
    if (s_monitor_ref_jpeg == NULL) {
        monitor_set_reference(cur_jpeg, cur_len);
        xSemaphoreGive(s_monitor_mutex);
        heap_caps_free(cur_jpeg);
        ESP_LOGI(TAG, "monitor reference frame saved");
        return;
    }

    /* Copy reference frame data while holding the lock, so it remains valid
     * even if monitoring is stopped concurrently. */
    uint8_t *ref_jpeg = NULL;
    size_t ref_len = 0;
    if (s_monitor_ref_jpeg != NULL && s_monitor_ref_jpeg_len > 0) {
        ref_jpeg = (uint8_t *)heap_caps_malloc(s_monitor_ref_jpeg_len, MALLOC_CAP_SPIRAM);
        if (ref_jpeg != NULL) {
            memcpy(ref_jpeg, s_monitor_ref_jpeg, s_monitor_ref_jpeg_len);
            ref_len = s_monitor_ref_jpeg_len;
        }
    }
    xSemaphoreGive(s_monitor_mutex);

    if (ref_jpeg == NULL) {
        ESP_LOGE(TAG, "failed to copy reference frame for comparison");
        heap_caps_free(cur_jpeg);
        return;
    }

    /* Ask LLM to compare reference and current frames. */
    const char *prompt = "第一张图是之前的场景，第二张图是现在的场景。"
                         "请比较两张图片。如果环境没有明显变化，请只回复 'NO_CHANGE'；"
                         "如果有变化，请用一句话简要描述变化内容。";

    cloud_response_t vlm_result = {0};
    ret = cloud_vlm_ask_with_reference(prompt,
                                       ref_jpeg, ref_len,
                                       cur_jpeg, cur_len,
                                       &vlm_result);
    heap_caps_free(ref_jpeg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor VLM comparison failed: %s", esp_err_to_name(ret));
        heap_caps_free(cur_jpeg);
        return;
    }

    if (monitor_response_is_silent(vlm_result.text)) {
        ESP_LOGI(TAG, "monitor: no change detected, stay silent");
        cloud_response_free(&vlm_result);
        heap_caps_free(cur_jpeg);
        return;
    }

    ESP_LOGI(TAG, "monitor: change detected: %s", vlm_result.text);

    cloud_sync_alert(CLOUD_SYNC_ALERT_GUARD, 90, vlm_result.text);

    /* Update reference frame to current to avoid repeated alerts. */
    xSemaphoreTake(s_monitor_mutex, portMAX_DELAY);
    monitor_set_reference(cur_jpeg, cur_len);
    xSemaphoreGive(s_monitor_mutex);
    heap_caps_free(cur_jpeg);

    /* TTS alert. */
    int16_t *tts_pcm = NULL;
    size_t tts_samples = 0;
    ret = cloud_tts_speak(vlm_result.text, &tts_pcm, &tts_samples);
    cloud_response_free(&vlm_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "monitor TTS failed: %s", esp_err_to_name(ret));
        return;
    }
    audio_play_pcm(tts_pcm, tts_samples);
    cloud_pcm_free(tts_pcm);
}
#endif

#if CONFIG_ISP_AWB_ONESHOT_ENABLE
/**
 * @brief One-shot auto white balance using ISP AWB statistics + CCM.
 *
 * The SC2336 on the ESP32-P4 Function EV board tends to produce a greenish
 * image with the default ISP pipeline.  We sample the white patches from the
 * first processed frame, compute the average R/G and B/G ratios, and apply a
 * diagonal color-correction matrix to neutralise the cast.
 */
static esp_err_t isp_apply_awb_ccm(isp_proc_handle_t isp_proc)
{
    isp_awb_ctlr_t awb_ctlr = NULL;
    int w = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES;
    int h = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES;

    esp_isp_awb_config_t awb_cfg = {
        .sample_point = ISP_AWB_SAMPLE_POINT_AFTER_CCM,
        .window = {
            .top_left  = { .x = (uint32_t)(w * 0.2f), .y = (uint32_t)(h * 0.2f) },
            .btm_right = { .x = (uint32_t)(w * 0.8f), .y = (uint32_t)(h * 0.8f) },
        },
        .white_patch = {
            .luminance = { .min = 0, .max = 220 * 3 },
            .red_green_ratio  = { .min = 0.0f, .max = 3.999f },
            .blue_green_ratio = { .min = 0.0f, .max = 3.999f },
        },
    };

    esp_err_t ret = esp_isp_new_awb_controller(isp_proc, &awb_cfg, &awb_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AWB controller creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_isp_awb_controller_enable(awb_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AWB controller enable failed: %s", esp_err_to_name(ret));
        goto del_awb;
    }

    isp_awb_stat_result_t stat = {};
    ret = esp_isp_awb_controller_get_oneshot_statistics(awb_ctlr, 2000, &stat);
    if (ret == ESP_OK && stat.white_patch_num > 0) {
        float n = (float)stat.white_patch_num;
        float avg_r = stat.sum_r / n;
        float avg_g = stat.sum_g / n;
        float avg_b = stat.sum_b / n;

        float r_gain = (avg_g > 0.0f) ? (avg_g / avg_r) : 1.0f;
        float b_gain = (avg_g > 0.0f) ? (avg_g / avg_b) : 1.0f;

        if (r_gain < 0.25f) r_gain = 0.25f;
        if (r_gain > 4.0f)  r_gain = 4.0f;
        if (b_gain < 0.25f) b_gain = 0.25f;
        if (b_gain > 4.0f)  b_gain = 4.0f;

        esp_isp_ccm_config_t ccm = {
            .matrix = {
                { r_gain, 0.0f, 0.0f },
                { 0.0f,   1.0f, 0.0f },
                { 0.0f,   0.0f, b_gain }
            },
            .saturation = true,
        };

        ESP_LOGI(TAG, "AWB stats: patches=%" PRIu32 " avg(R,G,B)=(%.2f,%.2f,%.2f) -> CCM(R,B)=(%.3f,%.3f)",
                 stat.white_patch_num, avg_r, avg_g, avg_b, r_gain, b_gain);

        ret = esp_isp_ccm_configure(isp_proc, &ccm);
        if (ret == ESP_OK) {
            ret = esp_isp_ccm_enable(isp_proc);
        }
    } else {
        ESP_LOGW(TAG, "AWB stats unavailable (%s) or no white patches (%" PRIu32 "), using fallback CCM",
                 esp_err_to_name(ret), stat.white_patch_num);
        esp_isp_ccm_config_t ccm = {
            .matrix = {
                { 1.15f, 0.08f, 0.0f },
                { 0.0f,  0.88f, 0.0f },
                { 0.0f,  0.08f, 1.15f }
            },
            .saturation = true,
        };
        esp_isp_ccm_configure(isp_proc, &ccm);
        esp_isp_ccm_enable(isp_proc);
        ret = ESP_OK;
    }

    esp_isp_awb_controller_disable(awb_ctlr);
del_awb:
    esp_isp_del_awb_controller(awb_ctlr);
    return ret;
}
#endif /* CONFIG_ISP_AWB_ONESHOT_ENABLE */

static esp_err_t isp_apply_color_correction(isp_proc_handle_t isp_proc)
{
#if CONFIG_ISP_AWB_ONESHOT_ENABLE
    return isp_apply_awb_ccm(isp_proc);
#else
    esp_isp_ccm_config_t ccm = {
        .matrix = {
            { 1.15f, 0.08f, 0.0f },
            { 0.0f,  0.88f, 0.0f },
            { 0.0f,  0.08f, 1.15f }
        },
        .saturation = true,
    };
    esp_err_t ret = esp_isp_ccm_configure(isp_proc, &ccm);
    if (ret == ESP_OK) {
        ret = esp_isp_ccm_enable(isp_proc);
    }
    return ret;
#endif
}

void app_main(void)
{
    esp_err_t ret = ESP_FAIL;

    s_cam_capture_mutex = xSemaphoreCreateMutex();
    s_reid_mutex = xSemaphoreCreateMutex();
    monitor_init();
    cloud_api_init();
    tts_worker_start();

    //---------------Wi-Fi & Network Init------------------//
    ret = wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed, continuing without network");
    } else {
        /* cloud_sync temporarily disabled to reduce memory pressure while
         * debugging TTS playback stability. */
        // cloud_sync_init();
        http_test_request();
    }

    //---------------UI Overlay Init------------------//
    ui_manager_init();
    s_ui_model = (ui_model_t){
        .wifi_connected = (ret == ESP_OK),
        .camera_ok = false,
        .privacy_on = false,
        .learn_score = 86,
        .risk_level = 1,
        .seat_state = "检测中",
        .screen_state = "等待课程开始",
        .pose_state = "检测中",
        .event_type = "学习状态提醒",
        .event_message = "准备好后就可以开始学习。",
        .summary_title = "今日课程",
        .summary_text = "1. 等待课程内容同步\n2. 准备进入专注守护",
        .study_seconds = 0,
        .event_duration_seconds = 0,
        .homework_text = "暂无作业提醒",
        .course_title = "智能伴学课程",
        .identity_name = "",
        .current_focus_seconds = 0,
        .total_focus_seconds = 0,
    };
    ui_manager_update(&s_ui_model);
    ui_manager_show_page(UI_PAGE_IDLE);

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
    void *frame_buffer = NULL;
    size_t frame_buffer_size = 0;

    //mipi ldo
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
        .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    /**
     * @background
     * Sensor use RAW8
     * ISP convert to RGB565
     */
    //---------------DSI Init------------------//
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 4)
    example_dsi_alloc_config_t dsi_alloc_config = EXAMPLE_DSI_ALLOC_CONFIG_DEFAULT();
    example_dsi_resource_alloc(&dsi_alloc_config, &mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, &frame_buffer, NULL);
#else
    example_dsi_resource_alloc(&mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, &frame_buffer);
#endif
    /* The DSI panel buffer is what the DPI controller scans out to the LCD.
     * We keep it as the LVGL render/display buffer.  The camera/ISP output is
     * redirected into a separate PSRAM buffer so the UI overlay can be
     * composited on top without being overwritten by the next frame. */
    s_display_buffer = frame_buffer;
    frame_buffer_size = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES * CONFIG_EXAMPLE_MIPI_DSI_DISP_VRES * EXAMPLE_RGB565_BITS_PER_PIXEL / 8;
    s_frame_buffer_size = frame_buffer_size;

    s_cam_buffer = heap_caps_aligned_alloc(64, frame_buffer_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (s_cam_buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate camera frame buffer (len=%zu)", frame_buffer_size);
        return;
    }
    s_frame_buffer = s_cam_buffer;
    memset(s_cam_buffer, 0x00, frame_buffer_size);
    esp_cache_msync(s_cam_buffer, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ESP_LOGD(TAG, "display_buffer=%p camera_buffer=%p size=%zu", frame_buffer, s_cam_buffer, frame_buffer_size);

    esp_cam_ctlr_trans_t new_trans = {
        .buffer = s_cam_buffer,
        .buflen = frame_buffer_size,
    };

    //--------Camera Sensor and SCCB Init-----------//
    example_sensor_handle_t sensor_handle = {
        .sccb_handle = NULL,
        .i2c_bus_handle = NULL,
    };
    example_sensor_config_t cam_sensor_config = {
        .i2c_port_num = I2C_NUM_0,
        .i2c_sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,
        .i2c_scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name = EXAMPLE_CAM_FORMAT,
    };
    example_sensor_init(&cam_sensor_config, &sensor_handle);
    ESP_LOGI(TAG, "sensor init done");

    s_ui_model.camera_ok = true;
    ui_manager_update(&s_ui_model);

    //---------------Audio Init------------------//
    ret = audio_init(sensor_handle.i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed, continuing without audio");
    } else {
#if CONFIG_AUDIO_ENABLE_LOOPBACK_TEST
        ESP_LOGI(TAG, "starting audio loopback test task");
        xTaskCreate(audio_loopback_test, "audio_loopback", 8192, NULL, 5, NULL);
#elif CONFIG_WAKE_WORD_ENABLED
        ESP_LOGI(TAG, "initializing wake word engine");
        ret = wake_word_init();
        if (ret == ESP_OK) {
            wake_word_register_callback(on_wake_word_detected);
            wake_word_start();
        } else {
            ESP_LOGE(TAG, "wake word init failed");
        }
#elif CONFIG_AUDIO_ENABLE_RECORD_TEST
        ESP_LOGI(TAG, "starting audio record test task");
        xTaskCreate(audio_record_test, "audio_record_test", 8192, NULL, 5, NULL);
#endif
    }

    //---------------PIR Sensor Init------------------//
#if CONFIG_PIR_SENSOR_ENABLED
    ret = pir_init((gpio_num_t)CONFIG_PIR_SENSOR_GPIO);
    if (ret == ESP_OK) {
        pir_register_callback(on_pir_motion_detected);
        pir_start();
    } else {
        ESP_LOGE(TAG, "PIR init failed");
    }
#endif

    //---------------CSI Init------------------//
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES,
        .v_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t cam_handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_config, &cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "csi init fail[%d]", ret);
        return;
    }
    s_cam_handle = cam_handle;
    s_cam_width = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES;
    s_cam_height = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES;

    /* Pre-allocate camera capture buffers so JPEG encoding does not need to
     * allocate large buffers at runtime. */
    ret = camera_capture_init(s_cam_width, s_cam_height);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera capture init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    if (esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &new_trans) != ESP_OK) {
        ESP_LOGE(TAG, "ops register fail");
        return;
    }

    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam_handle));
    ESP_LOGI(TAG, "csi enabled");

    //---------------ISP Init------------------//
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES,
        .v_res = CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES,
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_config, &isp_proc));
    ESP_ERROR_CHECK(esp_isp_enable(isp_proc));
    ESP_LOGI(TAG, "isp enabled");

    //---------------DPI Reset------------------//
    example_dpi_panel_reset(mipi_dpi_panel);

    //init to all white
    memset(frame_buffer, 0xFF, frame_buffer_size);
    esp_cache_msync((void *)frame_buffer, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    if (esp_cam_ctlr_start(cam_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Driver start fail");
        return;
    }
    ESP_LOGI(TAG, "csi started");

    example_dpi_panel_init(mipi_dpi_panel);
    ESP_LOGI(TAG, "dpi panel init done, entering loop");

    // Only need to submit the first transaction; the callbacks will keep
    // re-submitting the same buffer for every following frame.
    ret = esp_cam_ctlr_receive(cam_handle, &new_trans, pdMS_TO_TICKS(3000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "first receive fail, ret=0x%x", ret);
        return;
    }
    ESP_LOGI(TAG, "first frame received, entering idle loop");

    //---------------ISP Color Correction------------------//
    ret = isp_apply_color_correction(isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ISP color correction failed: %s", esp_err_to_name(ret));
    }

    //---------------LVGL Overlay------------------//
#if CONFIG_UI_MANAGER_USE_LVGL
    if (s_display_buffer && s_cam_buffer) {
        ui_lvgl_init(s_display_buffer, s_cam_buffer, s_cam_width, s_cam_height);
        ESP_LOGI(TAG, "LVGL overlay initialised");
        /* Sync the current UI model into the overlay now that it is ready. */
        ui_manager_update(&s_ui_model);
    }
#endif

    if (reid_model_init() != 0) {
        ESP_LOGE(TAG, "Re-ID model init failed");
    } else {
        ESP_LOGI(TAG, "Re-ID model initialized");
        reid_gallery_init();
        reid_gallery_set_threshold(REID_MATCH_THRESHOLD);
    }

    xTaskCreate(focus_tick_task, "focus_tick", 4096, NULL, 5, NULL);

    ret = human_detect_init();
    if (ret == ESP_OK) {
        human_register_left_callback(on_human_left);
        human_register_present_callback(on_human_present);
        human_register_left_reminder_callback(on_human_left_reminder);
        human_detect_register_frame_cb(on_detect_frame);
        ret = human_detect_register_frame_reader(app_human_frame_reader, NULL, s_cam_width, s_cam_height);
        if (ret == ESP_OK) {
            ret = human_detect_start();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "human detection start failed: %s", esp_err_to_name(ret));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#define MAX_HTTP_OUTPUT_BUFFER 1024

static void http_test_request(void)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    esp_http_client_config_t config = {
        .url = "https://httpbin.org/get",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http client open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);

    int total_read = 0;
    int r = 0;
    while (total_read < MAX_HTTP_OUTPUT_BUFFER - 1) {
        r = esp_http_client_read(client, local_response_buffer + total_read,
                                 MAX_HTTP_OUTPUT_BUFFER - 1 - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
    }
    local_response_buffer[total_read] = '\0';
    ESP_LOGI(TAG, "HTTP Response (%d bytes):\n%s", total_read, local_response_buffer);

    esp_http_client_cleanup(client);
}

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    esp_cam_ctlr_trans_t new_trans = *(esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = new_trans.buffer;
    trans->buflen = new_trans.buflen;

    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    BaseType_t woken = pdFALSE;
    ui_lvgl_notify_frame_ready(&woken);
    return woken == pdTRUE;
}
