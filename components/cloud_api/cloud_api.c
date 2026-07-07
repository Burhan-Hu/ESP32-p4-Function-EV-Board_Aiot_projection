/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
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
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    if (content_type != NULL) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }

    if (post_data != NULL && post_len > 0) {
        esp_http_client_set_post_field(client, post_data, post_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error status=%d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = content_length > 0 ? content_length : 4096;
    char *buf = (char *)malloc(read_len + 1);
    if (buf == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int r = 0;
    while (total_read < read_len) {
        r = esp_http_client_read(client, buf + total_read, read_len - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
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
                        "application/json", out_buf, out_len);
}

static const char *cjson_get_string(cJSON *root, const char *path)
{
    cJSON *item = cJSON_GetObjectItem(root, path);
    if (item && cJSON_IsString(item)) {
        return item->valuestring;
    }
    return NULL;
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

    char url[512];
    snprintf(url, sizeof(url),
             "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=%s&client_secret=%s",
             CONFIG_CLOUD_API_BAIDU_KEY, CONFIG_CLOUD_API_BAIDU_SECRET);

    char *resp = NULL;
    size_t resp_len = 0;
    esp_err_t err = http_request(url, HTTP_METHOD_POST, NULL, 0, NULL, &resp, &resp_len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
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

    cJSON *resp_json = cJSON_Parse(resp);
    free(resp);
    if (resp_json == NULL) {
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
    if (tex_encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "tex=%s&tok=%s&cuid=%s&ctp=1&lan=zh&spd=5&pit=5&vol=5&per=0&aue=4",
             tex_encoded, s_baidu_token, CONFIG_CLOUD_API_BAIDU_CUID);
    free(tex_encoded);

    char *resp = NULL;
    size_t resp_len = 0;
    err = http_request("https://tsn.baidu.com/text2audio", HTTP_METHOD_POST,
                       body, strlen(body),
                       "application/x-www-form-urlencoded",
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

    size_t samples = resp_len / sizeof(int16_t);
    int16_t *pcm = (int16_t *)malloc(resp_len);
    if (pcm == NULL) {
        free(resp);
        return ESP_ERR_NO_MEM;
    }

    memcpy(pcm, resp, resp_len);
    free(resp);

    *pcm_out = pcm;
    *sample_count_out = samples;
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Volcano Engine (Ark) VLM                                                   */
/* -------------------------------------------------------------------------- */

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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_CLOUD_API_VOLCANO_VLM_MODEL);

    cJSON *messages = cJSON_CreateArray();
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
            return ESP_ERR_NO_MEM;
        }

        const char *url_prefix = "data:image/jpeg;base64,";
        size_t url_len = strlen(url_prefix) + strlen(b64) + 1;
        char *url = (char *)malloc(url_len);
        if (url == NULL) {
            free(b64);
            cJSON_Delete(root);
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

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_CLOUD_API_VOLCANO_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Volcano VLM HTTP failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Volcano VLM status=%d, len=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "Volcano VLM HTTP error status=%d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = content_length > 0 ? content_length : 4096;
    char *resp = (char *)malloc(read_len + 1);
    if (resp == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int r = 0;
    while (total_read < read_len) {
        r = esp_http_client_read(client, resp + total_read, read_len - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
    }
    resp[total_read] = '\0';
    esp_http_client_cleanup(client);

    cJSON *resp_json = cJSON_Parse(resp);
    free(resp);
    if (resp_json == NULL) {
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Volcano VLM choices empty");
        cJSON_Delete(resp_json);
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    const char *content_text = cjson_get_string(message, "content");
    if (content_text == NULL) {
        ESP_LOGE(TAG, "Volcano VLM content empty");
        cJSON_Delete(resp_json);
        return ESP_FAIL;
    }

    *text_out = strdup(content_text);
    cJSON_Delete(resp_json);

    if (*text_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
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
    return baidu_tts(text, pcm_out, sample_count_out);
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
