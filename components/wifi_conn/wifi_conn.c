/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "wifi_conn.h"
#include "slave_ota.h"
#include "sdkconfig.h"
#include "esp_mac.h"

static const char *TAG = "wifi_conn";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;
static bool s_connected = false;

static bool mac_addr_is_zero(const uint8_t *mac)
{
    return (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 &&
            mac[3] == 0 && mac[4] == 0 && mac[5] == 0);
}

static void wifi_set_sta_mac(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK || mac_addr_is_zero(mac)) {
        ESP_LOGW(TAG, "Failed to read base MAC / MAC is zero, using locally administered fallback");
        mac[0] = 0x02;
        mac[1] = 0x00;
        mac[2] = 0x00;
        mac[3] = 0x00;
        mac[4] = 0x00;
        mac[5] = 0x01;
    }
    ESP_LOGI(TAG, "Setting STA MAC to " MACSTR, MAC2STR(mac));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac));
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGE(TAG, "Disconnected from AP, reason=%d (%s), RSSI=%d",
                 event->reason,
                 event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "4WAY_HANDSHAKE_TIMEOUT" :
                 event->reason == WIFI_REASON_AUTH_FAIL ? "AUTH_FAIL" :
                 event->reason == WIFI_REASON_NO_AP_FOUND ? "NO_AP_FOUND" :
                 event->reason == WIFI_REASON_ASSOC_FAIL ? "ASSOC_FAIL" :
                 event->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? "HANDSHAKE_TIMEOUT" :
                 event->reason == WIFI_REASON_CONNECTION_FAIL ? "CONNECTION_FAIL" : "UNKNOWN",
                 event->rssi);
        if (s_retry_num < CONFIG_WIFI_CONN_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(void)
{
    ESP_LOGI(TAG, "Initializing Wi-Fi STA...");

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Push C6 co-processor firmware over SDIO if it is missing or out of date.
     * On success this function restarts the host, so it never returns.
     * If no update is needed it returns immediately. */
    slave_ota_perform_if_needed();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_CONN_SSID,
            .password = CONFIG_WIFI_CONN_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .capable = false,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
#if CONFIG_WIFI_CONN_FORCE_STA_MAC
    wifi_set_sta_mac();
#endif
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "Connecting to SSID: %s", CONFIG_WIFI_CONN_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", CONFIG_WIFI_CONN_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", CONFIG_WIFI_CONN_SSID);
        return ESP_FAIL;
    }

    return ESP_FAIL;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_disconnect(void)
{
    s_connected = false;
    return esp_wifi_disconnect();
}
