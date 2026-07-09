/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_controller.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "led_controller";

#define LED_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define LED_MAX_DUTY        ((1U << 10) - 1U)

static SemaphoreHandle_t s_led_mutex = NULL;
static bool s_initialized = false;
static bool s_output_enabled = false;
static bool s_led_on = false;
static int s_brightness = CONFIG_LED_CONTROLLER_DEFAULT_BRIGHTNESS;

static int clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static uint32_t logical_percent_to_duty(int percent)
{
    uint32_t duty = (uint32_t)((clamp_percent(percent) * LED_MAX_DUTY) / 100);
#if !CONFIG_LED_CONTROLLER_ACTIVE_HIGH
    duty = LED_MAX_DUTY - duty;
#endif
    return duty;
}

static void led_apply_percent_locked(int percent)
{
    if (!s_output_enabled) {
        s_led_on = percent > 0;
        return;
    }

    uint32_t duty = logical_percent_to_duty(percent);
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set duty failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "update duty failed: %s", esp_err_to_name(ret));
        return;
    }

    s_led_on = percent > 0;
}

int led_controller_init(void)
{
    if (s_led_mutex == NULL) {
        s_led_mutex = xSemaphoreCreateMutex();
        if (s_led_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_led_mutex);
        return ESP_OK;
    }

    s_brightness = clamp_percent(CONFIG_LED_CONTROLLER_DEFAULT_BRIGHTNESS);

    if (CONFIG_LED_CONTROLLER_GPIO < 0) {
        s_initialized = true;
        s_output_enabled = false;
        s_led_on = false;
        xSemaphoreGive(s_led_mutex);
        ESP_LOGW(TAG, "LED output disabled, CONFIG_LED_CONTROLLER_GPIO=%d", CONFIG_LED_CONTROLLER_GPIO);
        return ESP_OK;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)CONFIG_LED_CONTROLLER_GPIO)) {
        xSemaphoreGive(s_led_mutex);
        ESP_LOGE(TAG, "invalid LED GPIO: %d", CONFIG_LED_CONTROLLER_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_DUTY_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_LED_CONTROLLER_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_led_mutex);
        ESP_LOGE(TAG, "timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num = CONFIG_LED_CONTROLLER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = logical_percent_to_duty(0),
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_led_mutex);
        ESP_LOGE(TAG, "channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_output_enabled = true;
    s_led_on = false;
    xSemaphoreGive(s_led_mutex);

    ESP_LOGI(TAG, "LED PWM initialized on GPIO%d, freq=%d Hz, default=%d%%",
             CONFIG_LED_CONTROLLER_GPIO,
             CONFIG_LED_CONTROLLER_PWM_FREQ_HZ,
             s_brightness);
    return ESP_OK;
}

void led_turn_on(void)
{
    if (!s_initialized && led_controller_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    int percent = s_brightness > 0 ? s_brightness : CONFIG_LED_CONTROLLER_DEFAULT_BRIGHTNESS;
    percent = clamp_percent(percent);
    if (percent == 0) {
        percent = 100;
    }
    led_apply_percent_locked(percent);
    xSemaphoreGive(s_led_mutex);
}

void led_turn_off(void)
{
    if (!s_initialized && led_controller_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    led_apply_percent_locked(0);
    xSemaphoreGive(s_led_mutex);
}

void led_set_brightness(int percent)
{
    if (!s_initialized && led_controller_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    s_brightness = clamp_percent(percent);
    if (s_led_on) {
        led_apply_percent_locked(s_brightness);
    }
    xSemaphoreGive(s_led_mutex);
}

bool led_is_on(void)
{
    if (s_led_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    bool on = s_led_on;
    xSemaphoreGive(s_led_mutex);
    return on;
}
