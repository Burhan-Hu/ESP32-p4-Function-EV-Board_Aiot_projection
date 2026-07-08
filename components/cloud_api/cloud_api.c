/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "cloud_api.h"

static const char *TAG = "cloud_api";

static char s_baidu_token[256] = {0};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

#if CONFIG_CLOUD_API_USE_MOCK
static esp_err_t response_set_text(cloud_response_t *out, const char *text)
{
    if (out == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(text);
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf, text, len + 1);
    out->text = buf;
    out->len = len;
    return ESP_OK;
}
#endif

static char *base64_encode(const uint8_t *data, size_t len)
{
    size_t out_len = 0;
    mbedtls_base64_encode(NULL, 0, &out_len, data, len);

    char *out = (char *)malloc(out_len + 1);
    if (out == NULL) {
        return NULL;
    }

    size_t written = 0;
    mbedtls_base64_encode((uint8_t *)out, out_len, &written, data, len);
    out[written] = '\0';
    return out;
}

static int url_encode_byte(char *out, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    if ((byte >= 'a' && byte <= 'z') ||
        (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') ||
        byte == '-' || byte == '_' || byte == '.' || byte == '~') {
        *out = (char)byte;
        return 1;
    }
    out[0] = '%';
    out[1] = hex[byte >> 4];
    out[2] = hex[byte & 0x0F];
    return 3;
}

static char *url_encode(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    size_t max_len = strlen(text) * 3 + 1;
    char *out = (char *)malloc(max_len);
    if (out == NULL) {
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        j += url_encode_byte(&out[j], (uint8_t)text[i]);
    }
    out[j] = '\0';
    return out;
}

static esp_err_t http_request(const char *url,
                              esp_http_client_method_t method,
                              const char *post_data,
                              size_t post_len,
                              const char *content_type,
                              const char *accept_type,
                              char **out_buf,
                              size_t *out_len)
{
    if (url == NULL || out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "curl/8.0.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    if (content_type != NULL) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }

    if (accept_type != NULL) {
        esp_http_client_set_header(client, "Accept", accept_type);
    }

    /* Open connection and send request headers. write_len tells the client
     * how many bytes of body will follow. */
    int write_len = (post_data != NULL && post_len > 0) ? (int)post_len : 0;
    esp_err_t err = esp_http_client_open(client, write_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    if (post_data != NULL && post_len > 0) {
        int wret = esp_http_client_write(client, post_data, (int)post_len);
        if (wret != (int)post_len) {
            ESP_LOGE(TAG, "HTTP write failed: %d/%zu", wret, post_len);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%lld", status, (long long)content_length);

    char *resp_content_type = NULL;
    char *resp_transfer_encoding = NULL;
    esp_http_client_get_header(client, "Content-Type", &resp_content_type);
    esp_http_client_get_header(client, "Transfer-Encoding", &resp_transfer_encoding);
    ESP_LOGI(TAG, "HTTP response headers: Content-Type=%s, Transfer-Encoding=%s",
             resp_content_type ? resp_content_type : "(none)",
             resp_transfer_encoding ? resp_transfer_encoding : "(none)");

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error status=%d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = content_length > 0 ? (int)content_length : 4096;
    char *buf = (char *)malloc(read_len + 1);
    if (buf == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int r = 0;
    int loop_count = 0;
    while (total_read < read_len) {
        r = esp_http_client_read(client, buf + total_read, read_len - total_read);
        ESP_LOGI(TAG, "HTTP read loop %d: r=%d, total=%d", loop_count++, r, total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;

        /* For chunked or unknown-length responses, grow buffer if full. */
        if (total_read >= read_len && content_length <= 0) {
            read_len *= 2;
            char *tmp = (char *)realloc(buf, read_len + 1);
            if (tmp == NULL) {
                ESP_LOGE(TAG, "failed to grow HTTP response buffer");
                free(buf);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = tmp;
        }
    }
    buf[total_read] = '\0';

    *out_buf = buf;
    *out_len = total_read;

    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t http_post_json(const char *url, const char *json_body,
                                char **out_buf, size_t *out_len)
{
    return http_request(url, HTTP_METHOD_POST, json_body, strlen(json_body),
                        "application/json", "application/json", out_buf, out_len);
}

static const char *cjson_get_string(cJSON *root, const char *path)
{
    cJSON *item = cJSON_GetObjectItem(root, path);
    if (item && cJSON_IsString(item)) {
        return item->valuestring;
    }
    return NULL;
}

/* Truncate a UTF-8 string to at most max_chars Unicode characters,
 * never cutting a multi-byte sequence. Returns a newly allocated string. */
static char *utf8_truncate(const char *text, size_t max_chars)
{
    if (text == NULL) {
        return NULL;
    }

    size_t char_count = 0;
    size_t byte_len = 0;
    const char *p = text;
    while (*p != '\0' && char_count < max_chars) {
        unsigned char c = (unsigned char)*p;
        size_t step = 1;
        if ((c & 0x80) != 0) {
            if ((c & 0xe0) == 0xc0) {
                step = 2;
            } else if ((c & 0xf0) == 0xe0) {
                step = 3;
            } else if ((c & 0xf8) == 0xf0) {
                step = 4;
            }
        }
        if (p[step - 1] == '\0' && step > 1) {
            /* incomplete multi-byte sequence at end of string */
            break;
        }
        p += step;
        byte_len += step;
        char_count++;
    }

    char *out = (char *)malloc(byte_len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, byte_len);
    out[byte_len] = '\0';
    return out;
}

/* -------------------------------------------------------------------------- */
/* Baidu ASR / TTS                                                            */
/* -------------------------------------------------------------------------- */

static esp_err_t baidu_refresh_token(void)
{
    if (s_baidu_token[0] != '\0') {
        return ESP_OK;
    }

    if (CONFIG_CLOUD_API_BAIDU_KEY[0] == '\0' ||
        CONFIG_CLOUD_API_BAIDU_SECRET[0] == '\0') {
        ESP_LOGE(TAG, "Baidu API key or secret not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Baidu OAuth 2.0 token endpoint expects parameters in the URL,
     * POST method, Content-Type: application/json, and an empty body.
     * Reference: https://ai.baidu.com/ai-doc/REFERENCE/Ck3dwjhhu
     */
    char *key_enc = url_encode(CONFIG_CLOUD_API_BAIDU_KEY);
    char *secret_enc = url_encode(CONFIG_CLOUD_API_BAIDU_SECRET);
    if (key_enc == NULL || secret_enc == NULL) {
        free(key_enc);
        free(secret_enc);
        return ESP_ERR_NO_MEM;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=%s&client_secret=%s",
             key_enc, secret_enc);
    free(key_enc);
    free(secret_enc);

    ESP_LOGI(TAG, "Baidu token request: key_len=%zu, secret_len=%zu",
             strlen(CONFIG_CLOUD_API_BAIDU_KEY), strlen(CONFIG_CLOUD_API_BAIDU_SECRET));
    ESP_LOGI(TAG, "Baidu token request URL (masked): https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=***&client_secret=***");

    char *resp = NULL;
    size_t resp_len = 0;
    esp_err_t err = http_request(url, HTTP_METHOD_POST, NULL, 0,
                                 "application/json", "application/json",
                                 &resp, &resp_len);
    if (err == ESP_OK && resp_len == 0) {
        ESP_LOGW(TAG, "Baidu token POST returned empty body, retrying with GET");
        free(resp);
        resp = NULL;
        err = http_request(url, HTTP_METHOD_GET, NULL, 0,
                           NULL, "application/json",
                           &resp, &resp_len);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Baidu token HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Baidu token response (%zu bytes): %s", resp_len, resp ? resp : "(null)");

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse Baidu token response as JSON");
        return ESP_FAIL;
    }

    const char *token = cjson_get_string(root, "access_token");
    if (token == NULL) {
        ESP_LOGE(TAG, "failed to parse Baidu access token");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(s_baidu_token, token, sizeof(s_baidu_token) - 1);
    s_baidu_token[sizeof(s_baidu_token) - 1] = '\0';
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Baidu token refreshed");
    return ESP_OK;
}

static esp_err_t baidu_asr(const int16_t *pcm, size_t sample_count, char **text_out)
{
    if (text_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = baidu_refresh_token();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Baidu token refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t pcm_bytes = sample_count * sizeof(int16_t);
    char *b64 = base64_encode((const uint8_t *)pcm, pcm_bytes);
    if (b64 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", "pcm");
    cJSON_AddNumberToObject(root, "rate", 16000);
    cJSON_AddNumberToObject(root, "channel", 1);
    cJSON_AddNumberToObject(root, "dev_pid", 1537);
    cJSON_AddStringToObject(root, "cuid", CONFIG_CLOUD_API_BAIDU_CUID);
    cJSON_AddStringToObject(root, "token", s_baidu_token);
    cJSON_AddStringToObject(root, "speech", b64);
    cJSON_AddNumberToObject(root, "len", (double)pcm_bytes);

    char *body = cJSON_PrintUnformatted(root);
    free(b64);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *resp = NULL;
    size_t resp_len = 0;
    err = http_post_json("https://vop.baidu.com/server_api", body, &resp, &resp_len);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ASR response (%zu bytes): %s", resp_len, resp);

    cJSON *resp_json = cJSON_Parse(resp);
    free(resp);
    if (resp_json == NULL) {
        ESP_LOGE(TAG, "failed to parse Baidu ASR response as JSON");
        return ESP_FAIL;
    }

    int err_no = 0;
    cJSON *err_item = cJSON_GetObjectItem(resp_json, "err_no");
    if (err_item && cJSON_IsNumber(err_item)) {
        err_no = err_item->valueint;
    }

    if (err_no != 0) {
        const char *err_msg = cjson_get_string(resp_json, "err_msg");
        ESP_LOGE(TAG, "Baidu ASR error: %d %s", err_no, err_msg ? err_msg : "");
        cJSON_Delete(resp_json);
        return ESP_FAIL;
    }

    cJSON *result_arr = cJSON_GetObjectItem(resp_json, "result");
    if (result_arr == NULL || !cJSON_IsArray(result_arr) ||
        cJSON_GetArraySize(result_arr) == 0) {
        ESP_LOGE(TAG, "Baidu ASR result empty");
        cJSON_Delete(resp_json);
        return ESP_FAIL;
    }

    cJSON *first = cJSON_GetArrayItem(result_arr, 0);
    if (first == NULL || !cJSON_IsString(first)) {
        ESP_LOGE(TAG, "Baidu ASR result format error");
        cJSON_Delete(resp_json);
        return ESP_FAIL;
    }

    *text_out = strdup(first->valuestring);
    cJSON_Delete(resp_json);

    if (*text_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t baidu_tts(const char *text, int16_t **pcm_out, size_t *sample_count_out)
{
    if (text == NULL || pcm_out == NULL || sample_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = baidu_refresh_token();
    if (err != ESP_OK) {
        return err;
    }

    char *tex_encoded = url_encode(text);
    char *tok_encoded = url_encode(s_baidu_token);
    if (tex_encoded == NULL || tok_encoded == NULL) {
        free(tex_encoded);
        free(tok_encoded);
        return ESP_ERR_NO_MEM;
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "tex=%s&tok=%s&cuid=%s&ctp=1&lan=zh&spd=7&pit=5&vol=12&per=0&aue=4",
             tex_encoded, tok_encoded, CONFIG_CLOUD_API_BAIDU_CUID);
    free(tex_encoded);
    free(tok_encoded);

    char *resp = NULL;
    size_t resp_len = 0;
    err = http_request("https://tsn.baidu.com/text2audio", HTTP_METHOD_POST,
                       body, strlen(body),
                       "application/x-www-form-urlencoded", NULL,
                       &resp, &resp_len);
    if (err != ESP_OK) {
        return err;
    }

    /* Baidu TTS returns binary PCM on success, JSON on error. */
    if (resp_len >= 2 && (resp[0] == '{' || resp[0] == '[')) {
        ESP_LOGE(TAG, "Baidu TTS error response: %.*s", (int)resp_len, resp);
        free(resp);
        return ESP_FAIL;
    }

    /* Take ownership of the HTTP response buffer to avoid an extra copy.
     * With CONFIG_SPIRAM_USE_MALLOC this should already be in PSRAM. */
    *pcm_out = (int16_t *)resp;
    *sample_count_out = resp_len / sizeof(int16_t);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Volcano Engine (Ark) VLM                                                   */
/* -------------------------------------------------------------------------- */

/* Simple text-only conversation history for VLM. Images are not kept in
 * history to save RAM and base64 encoding overhead. */

typedef struct {
    char *role; /* "user" or "assistant" */
    char *text;
} history_msg_t;

#ifndef CONFIG_CLOUD_API_VLM_HISTORY_MAX
#define CONFIG_CLOUD_API_VLM_HISTORY_MAX 6
#endif

static history_msg_t s_history[CONFIG_CLOUD_API_VLM_HISTORY_MAX];
static size_t s_history_count = 0;
static size_t s_history_next = 0;
static SemaphoreHandle_t s_history_mutex = NULL;

static void history_lock(void)
{
    if (s_history_mutex != NULL) {
        xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    }
}

static void history_unlock(void)
{
    if (s_history_mutex != NULL) {
        xSemaphoreGive(s_history_mutex);
    }
}

static void history_entry_free(history_msg_t *entry)
{
    if (entry == NULL) {
        return;
    }
    if (entry->role != NULL) {
        free(entry->role);
        entry->role = NULL;
    }
    if (entry->text != NULL) {
        free(entry->text);
        entry->text = NULL;
    }
}

static void history_clear_internal(void)
{
    for (size_t i = 0; i < CONFIG_CLOUD_API_VLM_HISTORY_MAX; i++) {
        history_entry_free(&s_history[i]);
    }
    s_history_count = 0;
    s_history_next = 0;
}

void cloud_vlm_history_clear(void)
{
    history_lock();
    history_clear_internal();
    history_unlock();
}

static esp_err_t history_append(const char *role, const char *text)
{
    if (role == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    history_msg_t *entry = &s_history[s_history_next];
    history_entry_free(entry);

    entry->role = strdup(role);
    entry->text = strdup(text);
    if (entry->role == NULL || entry->text == NULL) {
        history_entry_free(entry);
        return ESP_ERR_NO_MEM;
    }

    s_history_next = (s_history_next + 1) % CONFIG_CLOUD_API_VLM_HISTORY_MAX;
    if (s_history_count < CONFIG_CLOUD_API_VLM_HISTORY_MAX) {
        s_history_count++;
    }
    return ESP_OK;
}

void cloud_api_init(void)
{
    if (s_history_mutex == NULL) {
        s_history_mutex = xSemaphoreCreateMutex();
    }
}

/* Append history messages to the cJSON messages array in chronological order. */
static void history_to_json(cJSON *messages)
{
    if (messages == NULL || s_history_count == 0) {
        return;
    }

    size_t start = (s_history_next + CONFIG_CLOUD_API_VLM_HISTORY_MAX - s_history_count)
                   % CONFIG_CLOUD_API_VLM_HISTORY_MAX;
    for (size_t i = 0; i < s_history_count; i++) {
        size_t idx = (start + i) % CONFIG_CLOUD_API_VLM_HISTORY_MAX;
        history_msg_t *entry = &s_history[idx];
        if (entry->role == NULL || entry->text == NULL) {
            continue;
        }
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", entry->role);
        cJSON *content = cJSON_CreateArray();
        cJSON *text_item = cJSON_CreateObject();
        cJSON_AddStringToObject(text_item, "type", "text");
        cJSON_AddStringToObject(text_item, "text", entry->text);
        cJSON_AddItemToArray(content, text_item);
        cJSON_AddItemToObject(msg, "content", content);
        cJSON_AddItemToArray(messages, msg);
    }
}


static esp_err_t volcano_vlm_ask(const char *question,
                                 const uint8_t *jpeg_data, size_t jpeg_len,
                                 char **text_out)
{
    if (question == NULL || text_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (CONFIG_CLOUD_API_VOLCANO_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Volcano API key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    cloud_api_init();
    history_lock();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_CLOUD_API_VOLCANO_VLM_MODEL);

    cJSON *messages = cJSON_CreateArray();

    /* System instruction: keep answers short to save TTS time and memory. */
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
                            "You are a helpful assistant. Always reply in Chinese within 40 characters unless asked for detail.");
    cJSON_AddItemToArray(messages, sys_msg);

    /* Add previous text-only conversation turns. */
    history_to_json(messages);

    /* Add current user turn (text + optional image). */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content = cJSON_CreateArray();
    cJSON *text_item = cJSON_CreateObject();
    cJSON_AddStringToObject(text_item, "type", "text");
    cJSON_AddStringToObject(text_item, "text", question);
    cJSON_AddItemToArray(content, text_item);

    if (jpeg_data != NULL && jpeg_len > 0) {
        char *b64 = base64_encode(jpeg_data, jpeg_len);
        if (b64 == NULL) {
            cJSON_Delete(root);
            history_unlock();
            return ESP_ERR_NO_MEM;
        }

        const char *url_prefix = "data:image/jpeg;base64,";
        size_t url_len = strlen(url_prefix) + strlen(b64) + 1;
        char *url = (char *)malloc(url_len);
        if (url == NULL) {
            free(b64);
            cJSON_Delete(root);
            history_unlock();
            return ESP_ERR_NO_MEM;
        }
        snprintf(url, url_len, "%s%s", url_prefix, b64);
        free(b64);

        cJSON *img_item = cJSON_CreateObject();
        cJSON_AddStringToObject(img_item, "type", "image_url");
        cJSON *img_url = cJSON_CreateObject();
        cJSON_AddStringToObject(img_url, "url", url);
        free(url);
        cJSON_AddItemToObject(img_item, "image_url", img_url);
        cJSON_AddItemToArray(content, img_item);
    }

    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddNumberToObject(root, "max_tokens", 80);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *endpoint = CONFIG_CLOUD_API_VOLCANO_ENDPOINT;
    const char *suffix = "/chat/completions";
    char volcano_url[384];
    if (strstr(endpoint, suffix) != NULL) {
        snprintf(volcano_url, sizeof(volcano_url), "%s", endpoint);
    } else if (endpoint[strlen(endpoint) - 1] == '/') {
        snprintf(volcano_url, sizeof(volcano_url), "%s%s", endpoint, suffix + 1);
    } else {
        snprintf(volcano_url, sizeof(volcano_url), "%s%s", endpoint, suffix);
    }
    ESP_LOGI(TAG, "Volcano VLM request URL: %s", volcano_url);

    esp_http_client_config_t config = {
        .url = volcano_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "curl/8.0.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body);
        return ESP_FAIL;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_CLOUD_API_VOLCANO_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    size_t body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Volcano VLM HTTP open failed: %s", esp_err_to_name(err));
        free(body);
        esp_http_client_cleanup(client);
        return err;
    }

    int wret = esp_http_client_write(client, body, (int)body_len);
    free(body);
    if (wret != (int)body_len) {
        ESP_LOGE(TAG, "Volcano VLM HTTP write failed: %d/%zu", wret, body_len);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Volcano VLM status=%d, len=%lld", status, (long long)content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "Volcano VLM HTTP error status=%d", status);
        /* Read and log error response body for diagnostics. */
        char err_buf[512];
        int err_read = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (err_read > 0) {
            err_buf[err_read] = '\0';
            ESP_LOGE(TAG, "Volcano VLM error response: %s", err_buf);
        }
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = content_length > 0 ? (int)content_length : 4096;
    char *resp = (char *)malloc(read_len + 1);
    if (resp == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int r = 0;
    int loop_count = 0;
    while (total_read < read_len) {
        r = esp_http_client_read(client, resp + total_read, read_len - total_read);
        ESP_LOGI(TAG, "Volcano read loop %d: r=%d, total=%d", loop_count++, r, total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;

        if (total_read >= read_len && content_length <= 0) {
            read_len *= 2;
            char *tmp = (char *)realloc(resp, read_len + 1);
            if (tmp == NULL) {
                ESP_LOGE(TAG, "failed to grow Volcano response buffer");
                free(resp);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            resp = tmp;
        }
    }
    resp[total_read] = '\0';
    esp_http_client_cleanup(client);

    cJSON *resp_json = cJSON_Parse(resp);
    free(resp);
    if (resp_json == NULL) {
        history_unlock();
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Volcano VLM choices empty");
        cJSON_Delete(resp_json);
        history_unlock();
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    const char *content_text = cjson_get_string(message, "content");
    if (content_text == NULL) {
        ESP_LOGE(TAG, "Volcano VLM content empty");
        cJSON_Delete(resp_json);
        history_unlock();
        return ESP_FAIL;
    }

    *text_out = strdup(content_text);
    cJSON_Delete(resp_json);

    if (*text_out == NULL) {
        history_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Save this turn to history. Images are not kept in history. */
    history_append("user", question);
    history_append("assistant", *text_out);
    history_unlock();

    return ESP_OK;
}

static esp_err_t volcano_vlm_ask_with_reference(const char *question,
                                                const uint8_t *ref_jpeg_data, size_t ref_jpeg_len,
                                                const uint8_t *cur_jpeg_data, size_t cur_jpeg_len,
                                                char **text_out)
{
    if (question == NULL || text_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (CONFIG_CLOUD_API_VOLCANO_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Volcano API key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    cloud_api_init();
    history_lock();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_CLOUD_API_VOLCANO_VLM_MODEL);

    cJSON *messages = cJSON_CreateArray();

    /* System instruction: keep answers short to save TTS time and memory. */
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
                            "You are a helpful assistant. Always reply in Chinese within 40 characters unless asked for detail.");
    cJSON_AddItemToArray(messages, sys_msg);

    /* Add previous text-only conversation turns. */
    history_to_json(messages);

    /* Add current user turn (text + reference image + current image). */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content = cJSON_CreateArray();

    /* Text prompt instructing the model to compare and stay silent if no change. */
    cJSON *text_item = cJSON_CreateObject();
    cJSON_AddStringToObject(text_item, "type", "text");
    cJSON_AddStringToObject(text_item, "text", question);
    cJSON_AddItemToArray(content, text_item);

    for (int img_idx = 0; img_idx < 2; img_idx++) {
        const uint8_t *jpeg_data = (img_idx == 0) ? ref_jpeg_data : cur_jpeg_data;
        size_t jpeg_len = (img_idx == 0) ? ref_jpeg_len : cur_jpeg_len;
        if (jpeg_data == NULL || jpeg_len == 0) {
            continue;
        }

        char *b64 = base64_encode(jpeg_data, jpeg_len);
        if (b64 == NULL) {
            cJSON_Delete(root);
            history_unlock();
            return ESP_ERR_NO_MEM;
        }

        const char *url_prefix = "data:image/jpeg;base64,";
        size_t url_len = strlen(url_prefix) + strlen(b64) + 1;
        char *url = (char *)malloc(url_len);
        if (url == NULL) {
            free(b64);
            cJSON_Delete(root);
            history_unlock();
            return ESP_ERR_NO_MEM;
        }
        snprintf(url, url_len, "%s%s", url_prefix, b64);
        free(b64);

        cJSON *img_item = cJSON_CreateObject();
        cJSON_AddStringToObject(img_item, "type", "image_url");
        cJSON *img_url = cJSON_CreateObject();
        cJSON_AddStringToObject(img_url, "url", url);
        free(url);
        cJSON_AddItemToObject(img_item, "image_url", img_url);
        cJSON_AddItemToArray(content, img_item);
    }

    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddNumberToObject(root, "max_tokens", 80);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        history_unlock();
        return ESP_ERR_NO_MEM;
    }

    const char *endpoint = CONFIG_CLOUD_API_VOLCANO_ENDPOINT;
    const char *suffix = "/chat/completions";
    char volcano_url[384];
    if (strstr(endpoint, suffix) != NULL) {
        snprintf(volcano_url, sizeof(volcano_url), "%s", endpoint);
    } else if (endpoint[strlen(endpoint) - 1] == '/') {
        snprintf(volcano_url, sizeof(volcano_url), "%s%s", endpoint, suffix + 1);
    } else {
        snprintf(volcano_url, sizeof(volcano_url), "%s%s", endpoint, suffix);
    }
    ESP_LOGI(TAG, "Volcano VLM request URL: %s", volcano_url);

    esp_http_client_config_t config = {
        .url = volcano_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "curl/8.0.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body);
        history_unlock();
        return ESP_FAIL;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_CLOUD_API_VOLCANO_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    size_t body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Volcano VLM HTTP open failed: %s", esp_err_to_name(err));
        free(body);
        esp_http_client_cleanup(client);
        history_unlock();
        return err;
    }

    int wret = esp_http_client_write(client, body, (int)body_len);
    free(body);
    if (wret != (int)body_len) {
        ESP_LOGE(TAG, "Volcano VLM HTTP write failed: %d/%zu", wret, body_len);
        esp_http_client_cleanup(client);
        history_unlock();
        return ESP_FAIL;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Volcano VLM status=%d, len=%lld", status, (long long)content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "Volcano VLM HTTP error status=%d", status);
        char err_buf[512];
        int err_read = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (err_read > 0) {
            err_buf[err_read] = '\0';
            ESP_LOGE(TAG, "Volcano VLM error response: %s", err_buf);
        }
        esp_http_client_cleanup(client);
        history_unlock();
        return ESP_FAIL;
    }

    int read_len = content_length > 0 ? (int)content_length : 4096;
    char *resp = (char *)malloc(read_len + 1);
    if (resp == NULL) {
        esp_http_client_cleanup(client);
        history_unlock();
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int r = 0;
    int loop_count = 0;
    while (total_read < read_len) {
        r = esp_http_client_read(client, resp + total_read, read_len - total_read);
        ESP_LOGI(TAG, "Volcano read loop %d: r=%d, total=%d", loop_count++, r, total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;

        if (total_read >= read_len && content_length <= 0) {
            read_len *= 2;
            char *tmp = (char *)realloc(resp, read_len + 1);
            if (tmp == NULL) {
                ESP_LOGE(TAG, "failed to grow Volcano response buffer");
                free(resp);
                esp_http_client_cleanup(client);
                history_unlock();
                return ESP_ERR_NO_MEM;
            }
            resp = tmp;
        }
    }
    resp[total_read] = '\0';
    esp_http_client_cleanup(client);

    cJSON *resp_json = cJSON_Parse(resp);
    free(resp);
    if (resp_json == NULL) {
        history_unlock();
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Volcano VLM choices empty");
        cJSON_Delete(resp_json);
        history_unlock();
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    const char *content_text = cjson_get_string(message, "content");
    if (content_text == NULL) {
        ESP_LOGE(TAG, "Volcano VLM content empty");
        cJSON_Delete(resp_json);
        history_unlock();
        return ESP_FAIL;
    }

    *text_out = strdup(content_text);
    cJSON_Delete(resp_json);

    if (*text_out == NULL) {
        history_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Save this turn to history. Images are not kept in history. */
    history_append("user", question);
    history_append("assistant", *text_out);
    history_unlock();

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t cloud_asr_transcribe(const int16_t *pcm, size_t sample_count,
                               cloud_response_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_CLOUD_API_USE_MOCK
    ESP_LOGI(TAG, "ASR mock: %zu samples received", sample_count);
    return response_set_text(out, "描述一下画面");
#else
    char *text = NULL;
    esp_err_t err = baidu_asr(pcm, sample_count, &text);
    if (err != ESP_OK) {
        return err;
    }
    out->text = text;
    out->len = strlen(text);
    ESP_LOGI(TAG, "ASR result: %s", text);
    return ESP_OK;
#endif
}

esp_err_t cloud_vlm_ask(const char *question,
                        const uint8_t *jpeg_data, size_t jpeg_len,
                        cloud_response_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_CLOUD_API_USE_MOCK
    ESP_LOGI(TAG, "VLM mock: question='%s', jpeg_len=%zu",
             question ? question : "(null)", jpeg_len);
    return response_set_text(out, "这是一个室内场景，我可以看到一些家具和电子设备。");
#else
    char *text = NULL;
    esp_err_t err = volcano_vlm_ask(question, jpeg_data, jpeg_len, &text);
    if (err != ESP_OK) {
        return err;
    }
    out->text = text;
    out->len = strlen(text);
    ESP_LOGI(TAG, "VLM result: %s", text);
    return ESP_OK;
#endif
}

esp_err_t cloud_vlm_ask_with_reference(const char *question,
                                       const uint8_t *ref_jpeg_data, size_t ref_jpeg_len,
                                       const uint8_t *cur_jpeg_data, size_t cur_jpeg_len,
                                       cloud_response_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_CLOUD_API_USE_MOCK
    ESP_LOGI(TAG, "VLM mock with reference: question='%s', ref_len=%zu, cur_len=%zu",
             question ? question : "(null)", ref_jpeg_len, cur_jpeg_len);
    return response_set_text(out, "画面有变化，我注意到有物体移动。");
#else
    char *text = NULL;
    esp_err_t err = volcano_vlm_ask_with_reference(question,
                                                   ref_jpeg_data, ref_jpeg_len,
                                                   cur_jpeg_data, cur_jpeg_len,
                                                   &text);
    if (err != ESP_OK) {
        return err;
    }
    out->text = text;
    out->len = strlen(text);
    ESP_LOGI(TAG, "VLM result: %s", text);
    return ESP_OK;
#endif
}

esp_err_t cloud_tts_speak(const char *text,
                          int16_t **pcm_out, size_t *sample_count_out)
{
    if (text == NULL || pcm_out == NULL || sample_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_CLOUD_API_USE_MOCK
    ESP_LOGI(TAG, "TTS mock: text='%s'", text);

    const int sample_rate = 16000;
    const int duration_ms = 1500;
    size_t samples = (size_t)((sample_rate * duration_ms) / 1000);

    int16_t *pcm = (int16_t *)malloc(samples * sizeof(int16_t));
    if (pcm == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const float freq = 800.0f;
    const float amplitude = 8000.0f;
    for (size_t i = 0; i < samples; i++) {
        pcm[i] = (int16_t)(sinf(2.0f * (float)M_PI * freq * (float)i / (float)sample_rate) * amplitude);
    }

    *pcm_out = pcm;
    *sample_count_out = samples;
    return ESP_OK;
#else
    /* Limit TTS input length to keep response short and avoid huge buffers. */
    const size_t max_tts_chars = 80;
    char *truncated = utf8_truncate(text, max_tts_chars);
    if (truncated == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(truncated) < strlen(text)) {
        ESP_LOGW(TAG, "TTS input truncated to %zu chars", max_tts_chars);
    }
    ESP_LOGI(TAG, "TTS text: %s", truncated);
    esp_err_t err = baidu_tts(truncated, pcm_out, sample_count_out);
    free(truncated);
    return err;
#endif
}

void cloud_response_free(cloud_response_t *resp)
{
    if (resp == NULL) {
        return;
    }
    if (resp->text != NULL) {
        free(resp->text);
        resp->text = NULL;
    }
    resp->len = 0;
}

void cloud_pcm_free(int16_t *pcm)
{
    if (pcm != NULL) {
        free(pcm);
    }
}
