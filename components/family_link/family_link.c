#include "family_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include "wifi_conn.h"

#define FAMILY_LINK_DEVICE_ID_LEN       24
#define FAMILY_LINK_COMMAND_ID_LEN      64
#define FAMILY_LINK_SHA256_HEX_LEN      65
#define FAMILY_LINK_URL_LEN             320
#define FAMILY_LINK_JSON_MAX_BYTES      2048
#define FAMILY_LINK_AUDIO_MAX_BYTES     (256U * 1024U)
#define FAMILY_LINK_MAX_PCM_SAMPLES     80000U
#define FAMILY_LINK_RECENT_CACHE_SIZE   8U
#define FAMILY_LINK_VALID_EPOCH_MIN     1700000000LL

static const char *TAG = "family_link";

typedef struct {
    char command_id[FAMILY_LINK_COMMAND_ID_LEN];
    char audio_url[FAMILY_LINK_URL_LEN];
    char sha256[FAMILY_LINK_SHA256_HEX_LEN];
    size_t audio_size;
} family_link_command_t;

typedef struct {
    char command_id[FAMILY_LINK_COMMAND_ID_LEN];
    char final_status[9];
    bool ack_pending;
    bool received_acked;
} family_link_recent_t;

static TaskHandle_t s_task;
static family_link_playback_cb_t s_playback_cb;
static void *s_playback_ctx;
static char s_device_id[FAMILY_LINK_DEVICE_ID_LEN];
static family_link_recent_t s_recent[FAMILY_LINK_RECENT_CACHE_SIZE];
static size_t s_recent_count;
static size_t s_recent_next;
static StaticTask_t s_task_buffer;
static StackType_t *s_task_stack;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t resolve_device_id(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(mac); ++i) {
        all_zero = all_zero && (mac[i] == 0);
    }
    if (all_zero) {
        return ESP_ERR_INVALID_STATE;
    }
    int written = snprintf(s_device_id, sizeof(s_device_id),
                           "esp32p4-%02X%02X%02X%02X%02X%02X",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return (written > 0 && (size_t)written < sizeof(s_device_id))
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t build_url(char *out, size_t out_len, const char *path)
{
    int written = snprintf(out, out_len, "%s%s",
                           CONFIG_FAMILY_LINK_SERVER_URL, path);
    return (written > 0 && (size_t)written < out_len)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_response_body(esp_http_client_handle_t client,
                                    char *buffer, size_t buffer_len,
                                    int64_t content_length)
{
    if (buffer == NULL || buffer_len < 2 || content_length < 0 ||
        content_length >= (int64_t)buffer_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t total = 0;
    unsigned eagain_count = 0;
    while (total < (size_t)content_length) {
        int read = esp_http_client_read(client, buffer + total,
                                        (int)((size_t)content_length - total));
        if (read == -ESP_ERR_HTTP_EAGAIN && eagain_count++ < 5) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (read <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)read;
    }
    buffer[total] = '\0';
    return esp_http_client_is_complete_data_received(client) ? ESP_OK : ESP_FAIL;
}

static esp_err_t post_json(const char *path, const char *json)
{
    char url[FAMILY_LINK_URL_LEN];
    esp_err_t err = build_url(url, sizeof(url), path);
    if (err != ESP_OK) {
        return err;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_FAMILY_LINK_HTTP_TIMEOUT_MS,
    };
    /* Only attach certificate bundle for HTTPS URLs. Plain HTTP must not
     * attempt TLS negotiation or the server will reset the connection. */
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, (int)strlen(json));
    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_heartbeat(void)
{
    int64_t epoch = (int64_t)time(NULL);
    if (epoch < FAMILY_LINK_VALID_EPOCH_MIN) {
        epoch = 0;
    }
    char json[96];
    int written = snprintf(json, sizeof(json),
                           "{\"device_id\":\"%s\",\"timestamp\":%lld}",
                           s_device_id, (long long)epoch);
    if (written <= 0 || (size_t)written >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return post_json("/api/heartbeat", json);
}

static esp_err_t parse_poll_json(const char *json, family_link_command_t *command)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsObject(object)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "command_id");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(object, "command_type");
    cJSON *audio_url = cJSON_GetObjectItemCaseSensitive(object, "audio_url");
    cJSON *audio_size = cJSON_GetObjectItemCaseSensitive(object, "audio_size");
    cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(object, "audio_sha256");
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    bool sha_is_hex = cJSON_IsString(sha256) &&
                      strlen(sha256->valuestring) == 64;
    if (sha_is_hex) {
        for (size_t i = 0; i < 64; ++i) {
            char ch = sha256->valuestring[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
                sha_is_hex = false;
                break;
            }
        }
    }
    if (cJSON_IsString(id) && cJSON_IsString(type) &&
        strcmp(type->valuestring, "VOICE_MESSAGE") == 0 &&
        cJSON_IsString(audio_url) && cJSON_IsNumber(audio_size) && sha_is_hex &&
        audio_size->valuedouble > 0 &&
        audio_size->valuedouble <= FAMILY_LINK_AUDIO_MAX_BYTES &&
        audio_size->valuedouble == (double)(size_t)audio_size->valuedouble &&
        strlcpy(command->command_id, id->valuestring,
                sizeof(command->command_id)) < sizeof(command->command_id) &&
        strlcpy(command->audio_url, audio_url->valuestring,
                sizeof(command->audio_url)) < sizeof(command->audio_url) &&
        strlcpy(command->sha256, sha256->valuestring,
                sizeof(command->sha256)) < sizeof(command->sha256)) {
        command->audio_size = (size_t)audio_size->valuedouble;
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t poll_command(family_link_command_t *command, bool *has_command)
{
    if (command == NULL || has_command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *has_command = false;
    memset(command, 0, sizeof(*command));
    char path[96];
    int written = snprintf(path, sizeof(path),
                           "/api/device/commands?device_id=%s", s_device_id);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[FAMILY_LINK_URL_LEN];
    esp_err_t err = build_url(url, sizeof(url), path);
    if (err != ESP_OK) {
        return err;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_FAMILY_LINK_HTTP_TIMEOUT_MS,
    };
    /* Only attach certificate bundle for HTTPS URLs. Plain HTTP must not
     * attempt TLS negotiation or the server will reset the connection. */
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 204) {
        esp_http_client_cleanup(client);
        return ESP_OK;
    }
    char response[FAMILY_LINK_JSON_MAX_BYTES];
    err = (status == 200)
              ? read_response_body(client, response, sizeof(response), content_length)
              : ESP_FAIL;
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_poll_json(response, command);
    if (err == ESP_OK) {
        *has_command = true;
    }
    return err;
}

static void digest_to_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static esp_err_t download_audio(const family_link_command_t *command,
                                uint8_t **out_audio, size_t *out_len,
                                bool *out_permanent_failure)
{
    if (command == NULL || out_audio == NULL || out_len == NULL ||
        out_permanent_failure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_audio = NULL;
    *out_len = 0;
    *out_permanent_failure = false;
    char url[FAMILY_LINK_URL_LEN];
    if (command->audio_url[0] != '/' || command->audio_url[1] == '/' ||
        strstr(command->audio_url, "/../") != NULL ||
        build_url(url, sizeof(url), command->audio_url) != ESP_OK) {
        *out_permanent_failure = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_FAMILY_LINK_HTTP_TIMEOUT_MS,
    };
    /* Only attach certificate bundle for HTTPS URLs. Plain HTTP must not
     * attempt TLS negotiation or the server will reset the connection. */
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length <= 0 ||
        content_length > FAMILY_LINK_AUDIO_MAX_BYTES ||
        (size_t)content_length != command->audio_size) {
        if (status == 200) {
            *out_permanent_failure = true;
        }
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t *audio = heap_caps_malloc((size_t)content_length,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    mbedtls_sha256_context sha;
    uint8_t digest[32] = {0};
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        heap_caps_free(audio);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    size_t total = 0;
    unsigned eagain_count = 0;
    while (total < (size_t)content_length) {
        int read = esp_http_client_read(client, (char *)audio + total,
                                        (int)((size_t)content_length - total));
        if (read == -ESP_ERR_HTTP_EAGAIN && eagain_count++ < 10) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (read <= 0 || mbedtls_sha256_update(&sha, audio + total,
                                               (size_t)read) != 0) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)read;
    }
    if (err == ESP_OK && (total != (size_t)content_length ||
        !esp_http_client_is_complete_data_received(client) ||
        mbedtls_sha256_finish(&sha, digest) != 0)) {
        err = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    esp_http_client_cleanup(client);
    char actual_sha[65] = {0};
    if (err == ESP_OK) {
        digest_to_hex(digest, actual_sha);
    }
    if (err != ESP_OK || strcmp(actual_sha, command->sha256) != 0) {
        if (err == ESP_OK) {
            *out_permanent_failure = true;
        }
        heap_caps_free(audio);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_audio = audio;
    *out_len = total;
    return ESP_OK;
}

static esp_err_t parse_pcm_wav(uint8_t *wav, size_t wav_len,
                               const int16_t **out_pcm,
                               size_t *out_sample_count)
{
    if (wav == NULL || out_pcm == NULL || out_sample_count == NULL ||
        wav_len < 12 || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0 || read_le32(wav + 4) + 8U != wav_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool have_fmt = false;
    bool have_data = false;
    const uint8_t *data = NULL;
    uint32_t data_size = 0;
    size_t offset = 12;
    while (offset < wav_len) {
        if (wav_len - offset < 8) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t chunk_size = read_le32(wav + offset + 4);
        size_t chunk_start = offset + 8;
        if ((size_t)chunk_size > wav_len - chunk_start) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t chunk_end = chunk_start + (size_t)chunk_size;
        if ((chunk_size & 1U) != 0 && chunk_end == wav_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t padded_end = chunk_end + (chunk_size & 1U);
        if (chunk_end < chunk_start || padded_end > wav_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (memcmp(wav + offset, "fmt ", 4) == 0) {
            if (have_fmt || chunk_size < 16 || read_le16(wav + chunk_start) != 1 ||
                read_le16(wav + chunk_start + 2) != 1 ||
                read_le32(wav + chunk_start + 4) != 16000 ||
                read_le32(wav + chunk_start + 8) != 32000 ||
                read_le16(wav + chunk_start + 12) != 2 ||
                read_le16(wav + chunk_start + 14) != 16) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            have_fmt = true;
        } else if (memcmp(wav + offset, "data", 4) == 0) {
            if (have_data) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            data = wav + chunk_start;
            data_size = chunk_size;
            have_data = true;
        }
        offset = padded_end;
    }
    size_t samples = data_size / sizeof(int16_t);
    if (offset != wav_len || !have_fmt || !have_data || data_size == 0 ||
        (data_size & 1U) != 0 || samples > FAMILY_LINK_MAX_PCM_SAMPLES ||
        ((uintptr_t)data & (sizeof(int16_t) - 1U)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_pcm = (const int16_t *)data;
    *out_sample_count = samples;
    return ESP_OK;
}

static esp_err_t ack_command(const char *command_id, const char *status,
                             const char *reason)
{
    char path[128];
    int path_len = snprintf(path, sizeof(path),
                            "/api/device/commands/%s/ack", command_id);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "status", status);
    if (reason != NULL && reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = post_json(path, json);
    free(json);
    return err;
}

static family_link_recent_t *find_recent(const char *command_id)
{
    for (size_t i = 0; i < s_recent_count; ++i) {
        if (strcmp(s_recent[i].command_id, command_id) == 0) {
            return &s_recent[i];
        }
    }
    return NULL;
}

static void remember_final_status(const char *command_id, const char *status,
                                  bool ack_pending, bool received_acked)
{
    family_link_recent_t *slot = &s_recent[s_recent_next];
    strlcpy(slot->command_id, command_id, sizeof(slot->command_id));
    strlcpy(slot->final_status, status, sizeof(slot->final_status));
    slot->ack_pending = ack_pending;
    slot->received_acked = received_acked;
    s_recent_next = (s_recent_next + 1U) % FAMILY_LINK_RECENT_CACHE_SIZE;
    if (s_recent_count < FAMILY_LINK_RECENT_CACHE_SIZE) {
        ++s_recent_count;
    }
}

static esp_err_t retry_pending_acks(void)
{
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < s_recent_count; ++i) {
        family_link_recent_t *recent = &s_recent[i];
        if (!recent->ack_pending) {
            continue;
        }
        esp_err_t err = ESP_OK;
        if (strcmp(recent->final_status, "PLAYED") == 0) {
            if (!recent->received_acked) {
                err = ack_command(recent->command_id, "RECEIVED", NULL);
                if (err == ESP_OK) {
                    recent->received_acked = true;
                }
            }
            if (err == ESP_OK) {
                err = ack_command(recent->command_id, "PLAYED", NULL);
            }
        } else {
            err = ack_command(recent->command_id, "FAILED",
                              "audio playback failed");
        }
        if (err == ESP_OK) {
            recent->ack_pending = false;
        } else if (first_error == ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}

static esp_err_t process_command(const family_link_command_t *command)
{
    family_link_recent_t *recent = find_recent(command->command_id);
    if (recent != NULL) {
        ESP_LOGW(TAG, "duplicate command %s suppressed", command->command_id);
        esp_err_t ack_err = ESP_OK;
        if (recent->ack_pending) {
            if (strcmp(recent->final_status, "PLAYED") == 0) {
                if (!recent->received_acked) {
                    ack_err = ack_command(command->command_id, "RECEIVED", NULL);
                    if (ack_err == ESP_OK) {
                        recent->received_acked = true;
                    }
                }
                if (ack_err == ESP_OK) {
                    ack_err = ack_command(command->command_id, "PLAYED", NULL);
                }
            } else {
                ack_err = ack_command(command->command_id, "FAILED",
                                      "duplicate previously failed");
            }
            if (ack_err == ESP_OK) {
                recent->ack_pending = false;
            }
        }
        return ack_err;
    }
    uint8_t *audio = NULL;
    size_t audio_len = 0;
    bool permanent_failure = false;
    esp_err_t err = download_audio(command, &audio, &audio_len,
                                   &permanent_failure);
    if (err != ESP_OK) {
        if (permanent_failure) {
            esp_err_t ack_err = ack_command(command->command_id, "FAILED",
                                            "audio metadata or checksum invalid");
            remember_final_status(command->command_id, "FAILED",
                                  ack_err != ESP_OK, false);
            return ack_err;
        }
        /* Transport and allocation failures are retryable. Do not turn a
         * transient outage into a permanent FAILED command. A server redelivery
         * will retry with exponential backoff and still cannot reach playback
         * before a verified download plus RECEIVED ACK. */
        ESP_LOGW(TAG, "audio download/verification deferred: %s",
                 esp_err_to_name(err));
        return err;
    }
    const int16_t *pcm = NULL;
    size_t sample_count = 0;
    err = parse_pcm_wav(audio, audio_len, &pcm, &sample_count);
    if (err != ESP_OK) {
        esp_err_t ack_err = ack_command(command->command_id, "FAILED",
                                        "invalid PCM WAV payload");
        remember_final_status(command->command_id, "FAILED", ack_err != ESP_OK,
                              false);
        heap_caps_free(audio);
        return ack_err;
    }
    err = ack_command(command->command_id, "RECEIVED", NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RECEIVED ACK failed; playback deferred");
        heap_caps_free(audio);
        return err;
    }
    err = s_playback_cb(pcm, sample_count, s_playback_ctx);
    if (err == ESP_OK) {
        esp_err_t ack_err = ack_command(command->command_id, "PLAYED", NULL);
        remember_final_status(command->command_id, "PLAYED", ack_err != ESP_OK,
                              true);
        if (ack_err != ESP_OK) {
            err = ack_err;
        }
    } else {
        esp_err_t ack_err = ack_command(command->command_id, "FAILED",
                                        "audio playback failed");
        remember_final_status(command->command_id, "FAILED", ack_err != ESP_OK,
                              true);
        if (ack_err != ESP_OK) {
            err = ack_err;
        }
    }
    heap_caps_free(audio);
    return err;
}

static void family_link_task(void *arg)
{
    (void)arg;
    uint32_t retry_ms = CONFIG_FAMILY_LINK_RETRY_INITIAL_MS;
    TickType_t last_heartbeat = 0;
    ESP_LOGI(TAG, "started for device %s", s_device_id);
    while (true) {
        if (!wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = retry_ms < CONFIG_FAMILY_LINK_RETRY_MAX_MS / 2
                           ? retry_ms * 2
                           : CONFIG_FAMILY_LINK_RETRY_MAX_MS;
            continue;
        }
        bool ok = true;
        TickType_t now = xTaskGetTickCount();
        if (retry_pending_acks() != ESP_OK) {
            ok = false;
        }
        if (last_heartbeat == 0 ||
            now - last_heartbeat >=
                pdMS_TO_TICKS(CONFIG_FAMILY_LINK_HEARTBEAT_INTERVAL_MS)) {
            if (send_heartbeat() == ESP_OK) {
                last_heartbeat = now;
            } else {
                ok = false;
            }
        }
        family_link_command_t command;
        bool has_command = false;
        esp_err_t err = poll_command(&command, &has_command);
        if (err != ESP_OK) {
            ok = false;
        } else if (has_command) {
            ESP_LOGI(TAG, "received command %s", command.command_id);
            if (process_command(&command) != ESP_OK) {
                ok = false;
            }
        }
        if (ok) {
            retry_ms = CONFIG_FAMILY_LINK_RETRY_INITIAL_MS;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_FAMILY_LINK_POLL_INTERVAL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = retry_ms < CONFIG_FAMILY_LINK_RETRY_MAX_MS / 2
                           ? retry_ms * 2
                           : CONFIG_FAMILY_LINK_RETRY_MAX_MS;
        }
    }
}

esp_err_t family_link_start(family_link_playback_cb_t playback_cb,
                            void *user_ctx)
{
#if !CONFIG_FAMILY_LINK_ENABLE
    (void)playback_cb;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (playback_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = resolve_device_id();
    if (err != ESP_OK) {
        return err;
    }
    s_playback_cb = playback_cb;
    s_playback_ctx = user_ctx;
    size_t stack_bytes = CONFIG_FAMILY_LINK_TASK_STACK_SIZE;
    s_task_stack = heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_task_stack == NULL) {
        s_playback_cb = NULL;
        s_playback_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_task = xTaskCreateStatic(family_link_task, "family_link", stack_bytes,
                               NULL, 4, s_task_stack, &s_task_buffer);
    if (s_task == NULL) {
        heap_caps_free(s_task_stack);
        s_task_stack = NULL;
        s_task = NULL;
        s_playback_cb = NULL;
        s_playback_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}
