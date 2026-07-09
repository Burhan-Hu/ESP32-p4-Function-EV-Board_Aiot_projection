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

static const char *TAG = "human_detect";

static SemaphoreHandle_t s_state_mutex = nullptr;
static TaskHandle_t s_task_handle = nullptr;

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

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static float confidence_threshold(void)
{
    return (float)CONFIG_HUMAN_DETECT_CONFIDENCE_THRESHOLD / 100.0f;
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

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (detected) {
        s_present_streak++;
        s_last_seen_ms = now;
        if (!s_present && s_present_streak >= CONFIG_HUMAN_DETECT_ENTER_CONFIRM_FRAMES) {
            changed = set_present_locked(true, now, nullptr, nullptr);
        }
    } else {
        s_present_streak = 0;
        if (s_present && s_last_seen_ms > 0 && now - s_last_seen_ms >= s_leave_timeout_ms) {
            changed = set_present_locked(false, now, &left_duration_ms, &left_cb);
        }
    }
    present_after_update = s_present;

    xSemaphoreGive(s_state_mutex);

    if (!changed) {
        return;
    }

#if CONFIG_HUMAN_DETECT_LED_AUTO_CONTROL
    if (present_after_update) {
        led_turn_on();
    } else {
        led_turn_off();
    }
#endif
    if (!present_after_update && left_cb != nullptr) {
        left_cb(left_duration_ms);
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
    for (const auto &result : results) {
        best_score = std::max(best_score, result.score);
    }
    if (best_score_out != nullptr) {
        *best_score_out = best_score;
    }
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
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_HUMAN_DETECT_INTERVAL_MS));
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
    uint8_t *new_frame = (uint8_t *)heap_caps_malloc(frame_len, MALLOC_CAP_SPIRAM);
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

    BaseType_t ret = xTaskCreate(human_detect_task,
                                 "human_detect",
                                 CONFIG_HUMAN_DETECT_TASK_STACK_SIZE,
                                 nullptr,
                                 CONFIG_HUMAN_DETECT_TASK_PRIORITY,
                                 &s_task_handle);
    if (ret != pdPASS) {
        s_task_handle = nullptr;
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
