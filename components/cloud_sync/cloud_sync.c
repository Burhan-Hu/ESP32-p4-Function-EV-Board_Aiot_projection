/*
 * Cloud sync worker: SNTP time sync + asynchronous HTTP event reporting.
 *
 * Events (focus start/end, safety alerts) are pushed into a small queue by
 * the caller and POSTed to a local cloud server by a dedicated task. All
 * sends are best-effort with a short timeout; failures are logged and
 * dropped so the detection/UI pipeline is never blocked.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "cloud_sync.h"

static const char *TAG = "cloud_sync";

#define SYNC_QUEUE_LEN     8
#define SYNC_TASK_STACK    6144
#define SYNC_TASK_PRIO     4
#define HTTP_TIMEOUT_MS    3000
#define NAME_MAX_LEN       32
#define DETAIL_MAX_LEN     128

typedef enum {
    SYNC_EVT_FOCUS_START = 0,
    SYNC_EVT_FOCUS_END,
    SYNC_EVT_ALERT,
} sync_evt_type_t;

typedef struct {
    sync_evt_type_t type;
    char     name[NAME_MAX_LEN];       /* person name, or alert type */
    char     detail[DETAIL_MAX_LEN];   /* alert detail text */
    long     start;                    /* epoch seconds */
    long     end;                      /* epoch seconds */
    uint32_t duration;                 /* seconds */
    int      confidence;               /* 0-100 */
} sync_event_t;

static QueueHandle_t s_queue = NULL;

/* -------------------------------------------------------------------------- */
/* SNTP                                                                       */
/* -------------------------------------------------------------------------- */

static void sntp_init_and_wait(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_CLOUD_SYNC_NTP_SERVER);
    esp_sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();

    /* Wait up to ~10s for the clock to be set (epoch > 2023-11-14). */
    for (int i = 0; i < 20; i++) {
        if (time(NULL) > 1700000000) {
            ESP_LOGI(TAG, "SNTP synced, epoch=%ld", (long)time(NULL));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "SNTP sync timed out; timestamps will be relative");
}

/* -------------------------------------------------------------------------- */
/* HTTP                                                                       */
/* -------------------------------------------------------------------------- */

static esp_err_t http_post_json(const char *path, const char *json)
{
    char url[160];
    snprintf(url, sizeof(url), "%s%s", CONFIG_CLOUD_SYNC_SERVER_URL, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, (int)strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "POST %s failed: err=%s status=%d",
                 path, esp_err_to_name(err), status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "POST %s ok: %s", path, json);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* JSON building + worker task                                                */
/* -------------------------------------------------------------------------- */

static char *build_json(const sync_event_t *evt)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    switch (evt->type) {
    case SYNC_EVT_FOCUS_START:
        cJSON_AddStringToObject(root, "name", evt->name);
        cJSON_AddNumberToObject(root, "start", (double)evt->start);
        break;
    case SYNC_EVT_FOCUS_END:
        cJSON_AddStringToObject(root, "name", evt->name);
        cJSON_AddNumberToObject(root, "start", (double)evt->start);
        cJSON_AddNumberToObject(root, "end", (double)evt->end);
        cJSON_AddNumberToObject(root, "duration", (double)evt->duration);
        break;
    case SYNC_EVT_ALERT:
    default:
        cJSON_AddStringToObject(root, "type", evt->name);
        cJSON_AddNumberToObject(root, "confidence", evt->confidence);
        cJSON_AddNumberToObject(root, "time", (double)evt->end);
        cJSON_AddStringToObject(root, "detail", evt->detail);
        break;
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void sync_task(void *arg)
{
    (void)arg;
    sync_event_t evt;
    while (1) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char *json = build_json(&evt);
        if (json == NULL) {
            continue;
        }
        switch (evt.type) {
        case SYNC_EVT_FOCUS_START:
            http_post_json("/api/focus/start", json);
            break;
        case SYNC_EVT_FOCUS_END:
            http_post_json("/api/focus/end", json);
            break;
        case SYNC_EVT_ALERT:
        default:
            http_post_json("/api/alert", json);
            break;
        }
        free(json);
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t cloud_sync_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;  /* already running */
    }
    s_queue = xQueueCreate(SYNC_QUEUE_LEN, sizeof(sync_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sntp_init_and_wait();
    if (xTaskCreate(sync_task, "cloud_sync", SYNC_TASK_STACK, NULL,
                    SYNC_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "cloud sync started, server=%s", CONFIG_CLOUD_SYNC_SERVER_URL);
    return ESP_OK;
}

void cloud_sync_focus_start(const char *name, time_t start_epoch)
{
    if (s_queue == NULL) {
        return;
    }
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_FOCUS_START;
    strlcpy(evt.name, name ? name : "", sizeof(evt.name));
    evt.start = (long)(start_epoch > 0 ? start_epoch : time(NULL));
    xQueueSend(s_queue, &evt, 0);  /* drop silently if the queue is full */
}

void cloud_sync_focus_end(const char *name, time_t start_epoch,
                          time_t end_epoch, uint32_t duration_sec)
{
    if (s_queue == NULL) {
        return;
    }
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_FOCUS_END;
    strlcpy(evt.name, name ? name : "", sizeof(evt.name));
    evt.start = (long)start_epoch;
    evt.end = (long)(end_epoch > 0 ? end_epoch : time(NULL));
    evt.duration = duration_sec;
    xQueueSend(s_queue, &evt, 0);
}

void cloud_sync_alert(const char *type, int confidence, const char *detail)
{
    if (s_queue == NULL) {
        return;
    }
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_ALERT;
    strlcpy(evt.name, type ? type : "unknown", sizeof(evt.name));
    strlcpy(evt.detail, detail ? detail : "", sizeof(evt.detail));
    evt.end = (long)time(NULL);
    evt.confidence = confidence;
    xQueueSend(s_queue, &evt, 0);
}
