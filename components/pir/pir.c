/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pir.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "pir";

static gpio_num_t s_pir_gpio = GPIO_NUM_NC;
static pir_callback_t s_pir_cb = NULL;
static bool s_running = false;

#define PIR_POLL_MS      100
#define PIR_DEBOUNCE_MS  500

static void pir_task(void *arg)
{
    (void)arg;
    int last_level = 0;
    uint32_t last_trigger = 0;

    ESP_LOGI(TAG, "PIR monitoring started on GPIO%d", s_pir_gpio);

    while (true) {
        int level = gpio_get_level(s_pir_gpio);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (level == 1 && last_level == 0 &&
            (now - last_trigger) > PIR_DEBOUNCE_MS) {
            last_trigger = now;
            ESP_LOGI(TAG, "motion detected");
            if (s_pir_cb != NULL) {
                s_pir_cb();
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(PIR_POLL_MS));
    }
}

esp_err_t pir_init(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "PIR disabled (GPIO_NUM_NC)");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(gpio), ESP_ERR_INVALID_ARG,
                        TAG, "invalid GPIO number %d", gpio);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    s_pir_gpio = gpio;
    ESP_LOGI(TAG, "initialized on GPIO%d", gpio);
    return ESP_OK;
}

void pir_register_callback(pir_callback_t cb)
{
    s_pir_cb = cb;
}

esp_err_t pir_start(void)
{
    if (s_pir_gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "not starting, GPIO not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(pir_task, "pir_task", 16384, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "failed to create PIR task");

    s_running = true;
    return ESP_OK;
}
