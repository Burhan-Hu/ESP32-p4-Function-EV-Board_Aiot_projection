/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "human_detect.h"

#include <algorithm>
#include <inttypes.h>
#include <list>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "dl_image_define.hpp"
#include "pedestrian_detect.hpp"
#include "led_controller.h"

#ifndef CONFIG_HUMAN_DETECT_LEAVE_REMINDER_MS
#define CONFIG_HUMAN_DETECT_LEAVE_REMINDER_MS 60000
#endif

/* Debug overrides while tuning: hard-code detection cadence and confidence
 * threshold here so adjustments only rebuild this file (no full re-configure).
 * Move back to Kconfig (CONFIG_HUMAN_DETECT_INTERVAL_MS /
 * CONFIG_HUMAN_DETECT_CONFIDENCE_THRESHOLD) once stable. */
#define HUMAN_DETECT_RUN_INTERVAL_MS    3000
#define HUMAN_DETECT_CONF_THRESHOLD_PCT 60

static const char *TAG = "human_detect";

static SemaphoreHandle_t s_state_mutex = nullptr;
static TaskHandle_t s_task_handle = nullptr;

/* Allocate the detection task stack from PSRAM to avoid exhausting internal
 * RAM after audio/LVGL allocations. */
static StaticTask_t s_human_detect_tcb;
static StackType_t *s_human_detect_stack = nullptr;

static PedestrianDetect *s_detector = nullptr;
static human_frame_reader_t s_frame_reader = nullptr;
static void *s_frame_reader_ctx = nullptr;
static int s_frame_width = 0;
static int s_frame_height = 0;
static uint8_t *s_frame_copy = nullptr;
static size_t s_frame_copy_len = 0;

static bool s_initialized = false;
static bool s_present = false;
static uint32_t s_leave_timeout_ms = CONFIG_HUMAN_DETECT_LEAVE_TIMEOUT_MS;
static uint32_t s_last_seen_ms = 0;
static uint32_t s_present_since_ms = 0;
static int s_present_streak = 0;
static human_left_callback_t s_left_cb = nullptr;
static human_present_callback_t s_present_cb = nullptr;
static human_left_reminder_callback_t s_reminder_cb = nullptr;
static bool s_reminder_armed = false;
static uint32_t s_left_at_ms = 0;

/* Latest detection result, protected by s_state_mutex. */
static int s_latest_x1 = 0;
static int s_latest_y1 = 0;
static int s_latest_x2 = 0;
static int s_latest_y2 = 0;
static float s_latest_score = 0.0f;
static bool s_latest_valid = false;

/* Per-frame callback registered by the app; invoked from the detect task. */
static human_detect_frame_cb_t s_frame_cb = nullptr;

/* Reject detections whose bbox is too small or too narrow, to suppress
 * false-positive boxes on plain backgrounds (walls, bedding, curtains). */
#define HUMAN_DETECT_MIN_BBOX_AREA_PCT  4.0f   /* bbox area >= 4% of frame */
#define HUMAN_DETECT_MIN_BBOX_WIDTH_PCT 12.0f  /* bbox width >= 12% of frame */

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static float confidence_threshold(void)
{
    return (float)HUMAN_DETECT_CONF_THRESHOLD_PCT / 100.0f;
}

static bool set_present_locked(bool present,
                               uint32_t now,
                               uint32_t *left_duration_ms,
                               human_left_callback_t *left_cb)
{
    if (s_present == present) {
        return false;
    }

    s_present = present;
    if (present) {
        s_present_since_ms = now;
        ESP_LOGI(TAG, "person present");
        return true;
    }

    uint32_t duration_ms = (s_present_since_ms > 0 && now >= s_present_since_ms)
                               ? (now - s_present_since_ms)
                               : 0;
    if (left_duration_ms != nullptr) {
        *left_duration_ms = duration_ms;
    }
    if (left_cb != nullptr) {
        *left_cb = s_left_cb;
    }
    ESP_LOGI(TAG, "person left, duration=%" PRIu32 " ms", duration_ms);
    return true;
}

static void update_presence(bool detected, uint32_t now)
{
    bool changed = false;
    bool present_after_update = false;
    uint32_t left_duration_ms = 0;
    human_left_callback_t left_cb = nullptr;
    human_present_callback_t present_cb = nullptr;
    bool fire_reminder = false;
    uint32_t absent_ms = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (detected) {
        s_present_streak++;
        s_last_seen_ms = now;
        if (!s_present && s_present_streak >= CONFIG_HUMAN_DETECT_ENTER_CONFIRM_FRAMES) {
            changed = set_present_locked(true, now, nullptr, nullptr);
            if (changed) {
                s_reminder_armed = false;
                present_cb = s_present_cb;
            }
        }
    } else {
        s_present_streak = 0;
        if (s_present && s_last_seen_ms > 0 && now - s_last_seen_ms >= s_leave_timeout_ms) {
            changed = set_present_locked(false, now, &left_duration_ms, &left_cb);
            if (changed) {
                s_left_at_ms = now;
                s_reminder_armed = (s_reminder_cb != nullptr);
            }
        }
    }
    present_after_update = s_present;

    /* One-shot reminder while the person remains absent. */
    if (!present_after_update && s_reminder_armed && s_reminder_cb != nullptr &&
        now - s_left_at_ms >= CONFIG_HUMAN_DETECT_LEAVE_REMINDER_MS) {
        fire_reminder = true;
        absent_ms = now - s_left_at_ms;
        s_reminder_armed = false; /* fire only once per absence */
    }

    xSemaphoreGive(s_state_mutex);

    if (!changed && !fire_reminder) {
        return;
    }

#if CONFIG_HUMAN_DETECT_LED_AUTO_CONTROL
    if (changed) {
        if (present_after_update) {
            led_turn_on();
        } else {
            led_turn_off();
        }
    }
#endif
    if (changed && !present_after_update && left_cb != nullptr) {
        left_cb(left_duration_ms);
    }
    if (changed && present_after_update && present_cb != nullptr) {
        present_cb();
    }
    if (fire_reminder && s_reminder_cb != nullptr) {
        s_reminder_cb(absent_ms);
    }
}

static bool read_frame(void)
{
    human_frame_reader_t reader = nullptr;
    void *ctx = nullptr;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    reader = s_frame_reader;
    ctx = s_frame_reader_ctx;
    xSemaphoreGive(s_state_mutex);

    if (reader == nullptr || s_frame_copy == nullptr || s_frame_copy_len == 0) {
        return false;
    }

    int width = 0;
    int height = 0;
    if (!reader(s_frame_copy, s_frame_copy_len, &width, &height, ctx)) {
        return false;
    }
    if (width != s_frame_width || height != s_frame_height) {
        ESP_LOGW(TAG, "frame size changed: got %dx%d, expected %dx%d",
                 width, height, s_frame_width, s_frame_height);
        return false;
    }
    return true;
}

static bool run_detection(float *best_score_out)
{
    if (s_detector == nullptr || s_frame_copy == nullptr) {
        return false;
    }

    dl::image::img_t img = {};
    img.data = s_frame_copy;
    img.width = (uint16_t)s_frame_width;
    img.height = (uint16_t)s_frame_height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    std::list<dl::detect::result_t> &results = s_detector->run(img);
    float best_score = 0.0f;
    const dl::detect::result_t *best_result = nullptr;
    for (const auto &result : results) {
        if (result.score > best_score) {
            best_score = result.score;
            best_result = &result;
        }
    }

    /* Reject small or narrow boxes: they are usually false positives on plain
     * backgrounds that would otherwise keep the presence state alive. */
    if (best_result && best_result->box.size() >= 4) {
        int bw = best_result->box[2] - best_result->box[0];
        int bh = best_result->box[3] - best_result->box[1];
        float frame_area = (float)s_frame_width * (float)s_frame_height;
        float box_area = (float)bw * (float)bh;
        bool too_small = (frame_area > 0.0f &&
                          box_area < frame_area * HUMAN_DETECT_MIN_BBOX_AREA_PCT / 100.0f);
        bool too_narrow = (s_frame_width > 0 &&
                           bw < (int)(s_frame_width * HUMAN_DETECT_MIN_BBOX_WIDTH_PCT / 100.0f));
        if (too_small || too_narrow) {
            best_result = nullptr;
            best_score = 0.0f;
        }
    }

    if (best_score_out != nullptr) {
        *best_score_out = best_score;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (best_result && best_result->box.size() >= 4) {
        s_latest_valid = true;
        s_latest_x1 = best_result->box[0];
        s_latest_y1 = best_result->box[1];
        s_latest_x2 = best_result->box[2];
        s_latest_y2 = best_result->box[3];
        s_latest_score = best_score;
    } else {
        s_latest_valid = false;
    }
    xSemaphoreGive(s_state_mutex);

    return best_score >= confidence_threshold();
}

static void human_detect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "human detection task started");

    while (true) {
        if (read_frame()) {
            float best_score = 0.0f;
            bool detected = run_detection(&best_score);
#if CONFIG_HUMAN_DETECT_LOG_EACH_RESULT
            ESP_LOGI(TAG, "pedestrian detect: detected=%d, best_score=%.3f", detected, best_score);
#endif
            update_presence(detected, now_ms());

            /* Per-frame callback: let the app refresh identity / presence UI
             * from the raw per-frame result instead of the debounced state. */
            human_detect_frame_cb_t cb = nullptr;
            int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
            float bscore = 0.0f;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            cb = s_frame_cb;
            if (detected && s_latest_valid) {
                bx1 = s_latest_x1;
                by1 = s_latest_y1;
                bx2 = s_latest_x2;
                by2 = s_latest_y2;
                bscore = s_latest_score;
            }
            xSemaphoreGive(s_state_mutex);
            if (cb != nullptr) {
                cb(detected, s_frame_copy, s_frame_width, s_frame_height,
                   bx1, by1, bx2, by2, bscore);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HUMAN_DETECT_RUN_INTERVAL_MS));
    }
}

int human_detect_init(void)
{
    if (s_state_mutex == nullptr) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_state_mutex);

    esp_err_t ret = (esp_err_t)led_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_detector = new PedestrianDetect();
    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "failed to create PedestrianDetect model");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_leave_timeout_ms = CONFIG_HUMAN_DETECT_LEAVE_TIMEOUT_MS;
    s_initialized = true;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "ESP-WHO pedestrian detector initialized, threshold=%.2f, leave_timeout=%" PRIu32 " ms",
             confidence_threshold(), s_leave_timeout_ms);
    return ESP_OK;
}

int human_detect_register_frame_reader(human_frame_reader_t reader,
                                       void *ctx,
                                       int width,
                                       int height)
{
    if (reader == nullptr || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t frame_len = (size_t)width * (size_t)height * 2U;
    /* Keep the frame copy 16-byte aligned for cache efficiency.  The SIMD
     * image-preprocessing path in esp-dl has been disabled for ESP32-P4 because
     * it crashes when reading from PSRAM, so this buffer only needs to be
     * CPU-accessible. */
    uint8_t *new_frame = (uint8_t *)heap_caps_aligned_alloc(16, frame_len, MALLOC_CAP_SPIRAM);
    if (new_frame == nullptr) {
        ESP_LOGE(TAG, "failed to allocate frame copy buffer, len=%zu", frame_len);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_frame_copy != nullptr) {
        heap_caps_free(s_frame_copy);
    }
    s_frame_copy = new_frame;
    s_frame_copy_len = frame_len;
    s_frame_reader = reader;
    s_frame_reader_ctx = ctx;
    s_frame_width = width;
    s_frame_height = height;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "frame reader registered: %dx%d RGB565", width, height);
    return ESP_OK;
}

int human_detect_start(void)
{
    if (!s_initialized || s_detector == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_frame_reader == nullptr || s_frame_copy == nullptr) {
        ESP_LOGE(TAG, "frame reader not registered");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_handle != nullptr) {
        return ESP_OK;
    }

    const uint32_t stack_bytes = CONFIG_HUMAN_DETECT_TASK_STACK_SIZE;
    s_human_detect_stack = (StackType_t *)heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM);
    if (s_human_detect_stack == nullptr) {
        ESP_LOGE(TAG, "failed to allocate human_detect task stack from PSRAM, len=%" PRIu32, stack_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_task_handle = xTaskCreateStatic(human_detect_task,
                                      "human_detect",
                                      stack_bytes / sizeof(StackType_t),
                                      nullptr,
                                      CONFIG_HUMAN_DETECT_TASK_PRIORITY,
                                      s_human_detect_stack,
                                      &s_human_detect_tcb);
    if (s_task_handle == nullptr) {
        heap_caps_free(s_human_detect_stack);
        s_human_detect_stack = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool human_is_present(void)
{
    if (s_state_mutex == nullptr) {
        return false;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool present = s_present;
    xSemaphoreGive(s_state_mutex);
    return present;
}

void human_set_leave_timeout(uint32_t ms)
{
    if (s_state_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_leave_timeout_ms = ms;
    xSemaphoreGive(s_state_mutex);
}

void human_register_left_callback(human_left_callback_t cb)
{
    if (s_state_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_left_cb = cb;
    xSemaphoreGive(s_state_mutex);
}

void human_register_present_callback(human_present_callback_t cb)
{
    if (s_state_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_present_cb = cb;
    xSemaphoreGive(s_state_mutex);
}

void human_register_left_reminder_callback(human_left_reminder_callback_t cb)
{
    if (s_state_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_reminder_cb = cb;
    xSemaphoreGive(s_state_mutex);
}

bool human_detect_get_latest_bbox(int *x1, int *y1, int *x2, int *y2, float *score)
{
    if (s_state_mutex == nullptr) {
        return false;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool valid = s_latest_valid;
    if (valid) {
        if (x1) *x1 = s_latest_x1;
        if (y1) *y1 = s_latest_y1;
        if (x2) *x2 = s_latest_x2;
        if (y2) *y2 = s_latest_y2;
        if (score) *score = s_latest_score;
    }
    xSemaphoreGive(s_state_mutex);
    return valid;
}

void human_detect_register_frame_cb(human_detect_frame_cb_t cb)
{
    if (s_state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_frame_cb = cb;
    xSemaphoreGive(s_state_mutex);
}
