/*
 * Cloud sync worker: SNTP time sync + asynchronous HTTP event reporting.
 *
 * Events (focus start/end, safety alerts) are pushed into a small queue by
 * the caller and POSTed to the configured Guardian server by a dedicated task.
 * All sends are best-effort with a short timeout; failures are logged and
 * dropped so the detection/UI pipeline is never blocked.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
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
    SYNC_EVT_FAMILY,
} sync_evt_type_t;

typedef struct {
    char     name[NAME_MAX_LEN];       /* person name, or alert type */
    char     detail[DETAIL_MAX_LEN];   /* alert detail text */
    long     start;                    /* epoch seconds */
    long     end;                      /* epoch seconds */
    uint32_t duration;                 /* seconds */
    int      confidence;               /* 0-100 */
} legacy_sync_event_t;

typedef struct {
    sync_evt_type_t type;
    union {
        legacy_sync_event_t legacy;
        family_event_t family;
    } data;
} sync_event_t;

static QueueHandle_t s_queue = NULL;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static cloud_sync_stats_t s_stats;

static uint32_t increment_counter(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (*counter < UINT32_MAX) {
        (*counter)++;
    }
    uint32_t value = *counter;
    portEXIT_CRITICAL(&s_stats_lock);
    return value;
}

static bool enqueue_event(const sync_event_t *event)
{
    if (s_queue == NULL) {
        uint32_t drops = increment_counter(&s_stats.disabled_drops);
        if (drops == 1 || (drops % 16U) == 0) {
            ESP_LOGW(TAG, "cloud sync disabled; dropped event (total=%" PRIu32 ")",
                     drops);
        }
        return false;
    }
    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        uint32_t drops = increment_counter(&s_stats.queue_full_drops);
        if (drops == 1 || (drops % 16U) == 0) {
            ESP_LOGW(TAG, "cloud sync queue full; dropped event (total=%" PRIu32 ")",
                     drops);
        }
        return false;
    }
    increment_counter(&s_stats.enqueued_events);
    return true;
}

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
    int written = snprintf(url, sizeof(url), "%s%s",
                           CONFIG_CLOUD_SYNC_SERVER_URL, path);
    if (written <= 0 || (size_t)written >= sizeof(url)) {
        ESP_LOGE(TAG, "server URL is too long for path %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    /* Only attach certificate bundle for HTTPS URLs. Plain HTTP must not
     * attempt TLS negotiation or the server will reset the connection. */
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
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
    const legacy_sync_event_t *legacy = &evt->data.legacy;
    switch (evt->type) {
    case SYNC_EVT_FOCUS_START:
        cJSON_AddStringToObject(root, "name", legacy->name);
        cJSON_AddNumberToObject(root, "start", (double)legacy->start);
        break;
    case SYNC_EVT_FOCUS_END:
        cJSON_AddStringToObject(root, "name", legacy->name);
        cJSON_AddNumberToObject(root, "start", (double)legacy->start);
        cJSON_AddNumberToObject(root, "end", (double)legacy->end);
        cJSON_AddNumberToObject(root, "duration", (double)legacy->duration);
        break;
    case SYNC_EVT_FAMILY: {
        const family_event_t *family = &evt->data.family;
        cJSON_AddNumberToObject(root, "schema_version", family->schema_version);
        cJSON_AddStringToObject(root, "event_id", family->event_id);
        cJSON_AddStringToObject(root, "device_id", family->device_id);
        cJSON_AddStringToObject(root, "event_type",
                               family_event_type_name(family->event_type));
        if (family->person_id > 0) {
            cJSON_AddNumberToObject(root, "person_id", family->person_id);
        } else {
            cJSON_AddNullToObject(root, "person_id");
        }
        if (family->person_name[0] != '\0') {
            cJSON_AddStringToObject(root, "person_name", family->person_name);
        } else {
            cJSON_AddNullToObject(root, "person_name");
        }
        if (family->session_id[0] != '\0') {
            cJSON_AddStringToObject(root, "session_id", family->session_id);
        } else {
            cJSON_AddNullToObject(root, "session_id");
        }
        cJSON_AddNumberToObject(root, "timestamp", (double)family->timestamp);
        cJSON_AddNumberToObject(root, "focus_seconds", family->focus_seconds);
        cJSON_AddNumberToObject(root, "absence_seconds", family->absence_seconds);
        if (family->confidence_valid) {
            cJSON_AddNumberToObject(root, "confidence", family->confidence);
        } else {
            cJSON_AddNullToObject(root, "confidence");
        }
        if (family->end_reason_valid) {
            cJSON_AddStringToObject(root, "end_reason",
                                   family_focus_end_reason_name(family->end_reason));
        } else {
            cJSON_AddNullToObject(root, "end_reason");
        }
        cJSON_AddStringToObject(root, "detail", family->detail);
        break;
    }
    case SYNC_EVT_ALERT:
    default:
        cJSON_AddStringToObject(root, "type", legacy->name);
        cJSON_AddNumberToObject(root, "confidence", legacy->confidence);
        cJSON_AddNumberToObject(root, "time", (double)legacy->end);
        cJSON_AddStringToObject(root, "detail", legacy->detail);
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
        case SYNC_EVT_FAMILY:
            http_post_json("/api/events", json);
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

bool cloud_sync_publish(const family_event_t *event)
{
    if (!family_event_validate(event)) {
        uint32_t drops = increment_counter(&s_stats.invalid_event_drops);
        ESP_LOGW(TAG, "invalid family event rejected (total=%" PRIu32 ")", drops);
        return false;
    }

    sync_event_t queued_event = {0};
    queued_event.type = SYNC_EVT_FAMILY;
    queued_event.data.family = *event;  /* fixed arrays: complete deep copy */
    return enqueue_event(&queued_event);
}

void cloud_sync_get_stats(cloud_sync_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    *out_stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

void cloud_sync_focus_start(const char *name, time_t start_epoch)
{
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_FOCUS_START;
    legacy_sync_event_t *legacy = &evt.data.legacy;
    strlcpy(legacy->name, name ? name : "", sizeof(legacy->name));
    legacy->start = (long)(start_epoch > 0 ? start_epoch : time(NULL));
    enqueue_event(&evt);
}

void cloud_sync_focus_end(const char *name, time_t start_epoch,
                          time_t end_epoch, uint32_t duration_sec)
{
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_FOCUS_END;
    legacy_sync_event_t *legacy = &evt.data.legacy;
    strlcpy(legacy->name, name ? name : "", sizeof(legacy->name));
    legacy->start = (long)start_epoch;
    legacy->end = (long)(end_epoch > 0 ? end_epoch : time(NULL));
    legacy->duration = duration_sec;
    enqueue_event(&evt);
}

void cloud_sync_alert(const char *type, int confidence, const char *detail)
{
    sync_event_t evt = {0};
    evt.type = SYNC_EVT_ALERT;
    legacy_sync_event_t *legacy = &evt.data.legacy;
    strlcpy(legacy->name, type ? type : "unknown", sizeof(legacy->name));
    strlcpy(legacy->detail, detail ? detail : "", sizeof(legacy->detail));
    legacy->end = (long)time(NULL);
    legacy->confidence = confidence;
    enqueue_event(&evt);
}
