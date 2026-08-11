/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reid_gallery.h"

#include <cmath>
#include <cstring>
#include <inttypes.h>
#include <array>
#include <cstdio>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "reid_gallery";

#define REID_GALLERY_MAGIC    0x52454944u  /* 'REID' */
#define REID_GALLERY_VERSION  1u
#define REID_GALLERY_PATH     "/spiffs/reid_gallery.bin"

static reid_person_t s_gallery[REID_GALLERY_MAX];
static float s_threshold = 0.65f;
static int s_next_id = 1;
static int s_enroll_slot = -1;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_spiffs_mounted = false;
static TaskHandle_t s_save_task = nullptr;
static volatile bool s_save_pending = false;

#define REID_SAVE_TASK_STACK_SIZE 4096

static void reid_gallery_save_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        reid_gallery_save();
    }
}

static void start_save_task(void)
{
    if (s_save_task != nullptr) {
        return;
    }
    BaseType_t ret = xTaskCreate(reid_gallery_save_task,
                                 "reid_save",
                                 REID_SAVE_TASK_STACK_SIZE,
                                 nullptr,
                                 3,
                                 &s_save_task);
    if (ret != pdPASS || s_save_task == nullptr) {
        ESP_LOGE(TAG, "failed to create gallery save task");
        s_save_task = nullptr;
    } else {
        ESP_LOGI(TAG, "gallery save task started");
    }
}

static void request_save(void)
{
    s_save_pending = true;
    if (s_save_task != nullptr) {
        xTaskNotifyGive(s_save_task);
    }
}

static void reid_gallery_save_locked(void);

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_entries;
    uint32_t feature_dim;
    int32_t  next_id;
    float    threshold;
    uint8_t  reserved[40];
} __attribute__((packed)) gallery_file_header_t;

static_assert(sizeof(gallery_file_header_t) == 64, "gallery header size mismatch");

typedef struct {
    int32_t  person_id;
    char     name[REID_MAX_NAME_LEN + 1];
    uint8_t  view;
    uint8_t  active;
    uint32_t total_focus_seconds;
    uint32_t hit_count;
    uint32_t last_seen_ms;
    float    feature[REID_FEATURE_DIM];
} __attribute__((packed)) gallery_file_entry_t;

static_assert(sizeof(gallery_file_entry_t) == (4 + 16 + 1 + 1 + 4 + 4 + 4 + REID_FEATURE_DIM * 4),
              "gallery entry size mismatch");

static esp_err_t ensure_spiffs(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spiffs_mounted = true;
    return ESP_OK;
}

static void l2_normalize(float *v)
{
    float sum = 0.0f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        sum += v[i] * v[i];
    }
    float norm = std::sqrt(sum) + 1e-8f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        v[i] /= norm;
    }
}

void reid_gallery_init(void)
{
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    start_save_task();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_gallery, 0, sizeof(s_gallery));
    s_next_id = 1;
    s_threshold = 0.65f;
    s_enroll_slot = -1;
    reid_gallery_load();
    xSemaphoreGive(s_mutex);
}

void reid_gallery_reset(void)
{
    if (s_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_gallery, 0, sizeof(s_gallery));
    s_next_id = 1;
    s_enroll_slot = -1;
    xSemaphoreGive(s_mutex);
    request_save();
    ESP_LOGI(TAG, "gallery reset");
}

void reid_gallery_set_threshold(float threshold)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_threshold = threshold;
    xSemaphoreGive(s_mutex);
}

int reid_gallery_count(void)
{
    int count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active) {
            ++count;
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}

const reid_person_t *reid_gallery_get(int idx)
{
    if (idx < 0 || idx >= REID_GALLERY_MAX) {
        return nullptr;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const reid_person_t *p = s_gallery[idx].active ? &s_gallery[idx] : nullptr;
    xSemaphoreGive(s_mutex);
    return p;
}

const reid_person_t *reid_gallery_find_by_id(int person_id)
{
    if (person_id <= 0) {
        return nullptr;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const reid_person_t *out = nullptr;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            out = &s_gallery[i];
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return out;
}

const char *reid_gallery_get_name(int person_id)
{
    const reid_person_t *p = reid_gallery_find_by_id(person_id);
    return p ? p->name : "";
}

int reid_gallery_find_by_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int found_id = -1;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && std::strncmp(s_gallery[i].name, name, REID_MAX_NAME_LEN) == 0) {
            found_id = s_gallery[i].person_id;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found_id;
}

bool reid_gallery_delete_person(int person_id)
{
    if (person_id <= 0) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            s_gallery[i].active = false;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (found) {
        request_save();
    }
    return found;
}

bool reid_gallery_reset_focus_time(int person_id)
{
    if (person_id <= 0) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            s_gallery[i].total_focus_seconds = 0;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (found) {
        request_save();
    }
    return found;
}

static float cosine_similarity(const float *a, const float *b)
{
    float dot = 0.0f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        dot += a[i] * b[i];
    }
    return dot;
}

int reid_gallery_query(const float *feature, float threshold, float *out_similarity)
{
    if (feature == nullptr) {
        return -1;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int best_idx = -1;
    float best_sim = -1.0f;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (!s_gallery[i].active) {
            continue;
        }
        float sim = cosine_similarity(feature, s_gallery[i].feature);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }
    int id = -1;
    if (best_idx >= 0 && best_sim >= threshold) {
        id = s_gallery[best_idx].person_id;
    }
    if (out_similarity) {
        *out_similarity = (best_idx >= 0) ? best_sim : 0.0f;
    }
    xSemaphoreGive(s_mutex);
    return id;
}

bool reid_gallery_match_person(int person_id, const float *feature, float *out_similarity)
{
    if (person_id <= 0 || feature == nullptr) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const reid_person_t *p = nullptr;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            p = &s_gallery[i];
            break;
        }
    }
    float sim = 0.0f;
    bool found = false;
    if (p != nullptr) {
        sim = cosine_similarity(feature, p->feature);
        found = true;
    }
    xSemaphoreGive(s_mutex);
    if (out_similarity) {
        *out_similarity = sim;
    }
    return found;
}

bool reid_gallery_update_person(int person_id, const float *feature, uint32_t now_ms, float *out_similarity)
{
    if (person_id <= 0 || feature == nullptr) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    reid_person_t *p = nullptr;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            p = &s_gallery[i];
            break;
        }
    }
    if (p == nullptr) {
        xSemaphoreGive(s_mutex);
        if (out_similarity) {
            *out_similarity = 0.0f;
        }
        return false;
    }
    float sim = cosine_similarity(feature, p->feature);
    const float alpha = 0.5f;
    for (int i = 0; i < REID_FEATURE_DIM; ++i) {
        p->feature[i] = (1.0f - alpha) * p->feature[i] + alpha * feature[i];
    }
    l2_normalize(p->feature);
    p->last_seen_ms = now_ms;
    p->hit_count += 1;
    xSemaphoreGive(s_mutex);
    if (out_similarity) {
        *out_similarity = sim;
    }
    ESP_LOGI(TAG, "updated %s (id=%d), sim=%.4f, hits=%" PRIu32,
             p->name, person_id, sim, p->hit_count);
    return true;
}

static int find_or_alloc_slot(void)
{
    int oldest = -1;
    uint32_t oldest_ms = 0xFFFFFFFFu;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (!s_gallery[i].active) {
            return i;
        }
        if (s_gallery[i].last_seen_ms < oldest_ms) {
            oldest_ms = s_gallery[i].last_seen_ms;
            oldest = i;
        }
    }
    return oldest;
}

int reid_gallery_update(const float *feature, uint32_t now_ms, float *out_similarity)
{
    if (feature == nullptr) {
        return -1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int best_idx = -1;
    float best_sim = -1.0f;

    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (!s_gallery[i].active) {
            continue;
        }
        float sim = cosine_similarity(feature, s_gallery[i].feature);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }

    if (best_idx >= 0 && best_sim >= s_threshold) {
        reid_person_t *p = &s_gallery[best_idx];
        /* Only absorb the new feature into the template when the similarity is
         * in the [0.85, 0.90] band: high enough to be confident it is the same
         * person, but not so high that it signals template self-feedback. This
         * stops background false-positive features from polluting the template
         * (the runaway sim -> 0.99 seen in testing). */
        if (best_sim >= 0.85f && best_sim <= 0.90f) {
            const float alpha = 0.5f;
            for (int i = 0; i < REID_FEATURE_DIM; ++i) {
                p->feature[i] = (1.0f - alpha) * p->feature[i] + alpha * feature[i];
            }
            l2_normalize(p->feature);
        }
        p->last_seen_ms = now_ms;
        p->hit_count += 1;
        if (out_similarity) {
            *out_similarity = best_sim;
        }
        int id = p->person_id;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "matched %s (id=%d), sim=%.4f, hits=%" PRIu32,
                 p->name, id, best_sim, p->hit_count);
        return id;
    }

    int slot = find_or_alloc_slot();
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        if (out_similarity) {
            *out_similarity = 0.0f;
        }
        ESP_LOGW(TAG, "gallery full, cannot register new person");
        return -1;
    }

    reid_person_t *p = &s_gallery[slot];
    p->active = true;
    p->person_id = s_next_id++;
    std::strncpy(p->name, "陌生人", sizeof(p->name));
    p->name[REID_MAX_NAME_LEN] = '\0';
    p->view = REID_VIEW_UNKNOWN;
    std::memcpy(p->feature, feature, sizeof(p->feature));
    p->total_focus_seconds = 0;
    p->last_seen_ms = now_ms;
    p->hit_count = 1;

    if (out_similarity) {
        *out_similarity = 0.0f;
    }
    int id = p->person_id;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "new person id=%d registered (gallery=%d/%d), best_sim=%.4f",
             id, reid_gallery_count(), REID_GALLERY_MAX, best_sim);
    return id;
}

void reid_gallery_add_focus_seconds(int person_id, uint32_t seconds)
{
    if (person_id <= 0 || seconds == 0) {
        return;
    }
    bool changed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        if (s_gallery[i].active && s_gallery[i].person_id == person_id) {
            s_gallery[i].total_focus_seconds += seconds;
            ESP_LOGI(TAG, "added %" PRIu32 "s focus to %s (id=%d), total=%" PRIu32 "s",
                     seconds, s_gallery[i].name, person_id, s_gallery[i].total_focus_seconds);
            changed = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (changed) {
        request_save();
    }
}

bool reid_gallery_enroll_start(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int slot = find_or_alloc_slot();
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    reid_person_t *p = &s_gallery[slot];
    p->active = false;
    p->person_id = 0;
    std::strncpy(p->name, name, REID_MAX_NAME_LEN);
    p->name[REID_MAX_NAME_LEN] = '\0';
    p->view = REID_VIEW_UNKNOWN;
    p->total_focus_seconds = 0;
    p->hit_count = 0;
    p->last_seen_ms = 0;
    std::memset(p->feature, 0, sizeof(p->feature));
    s_enroll_slot = slot;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "enrollment started for '%s' slot=%d", name, slot);
    return true;
}

bool reid_gallery_enroll_feed(const float *feature)
{
    if (feature == nullptr || s_enroll_slot < 0) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    reid_person_t *p = &s_gallery[s_enroll_slot];
    if (p->hit_count == 0) {
        std::memcpy(p->feature, feature, sizeof(p->feature));
    } else {
        const float alpha = 0.3f;
        for (int i = 0; i < REID_FEATURE_DIM; ++i) {
            p->feature[i] = (1.0f - alpha) * p->feature[i] + alpha * feature[i];
        }
        l2_normalize(p->feature);
    }
    p->hit_count += 1;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "enrollment feed %d samples", p->hit_count);
    return true;
}

int reid_gallery_enroll_finish(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_enroll_slot < 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }
    reid_person_t *p = &s_gallery[s_enroll_slot];
    if (p->hit_count == 0) {
        s_enroll_slot = -1;
        xSemaphoreGive(s_mutex);
        return -1;
    }
    p->active = true;
    p->person_id = s_next_id++;
    p->last_seen_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int id = p->person_id;
    s_enroll_slot = -1;
    ESP_LOGI(TAG, "enrollment finished: %s id=%d", p->name, id);
    xSemaphoreGive(s_mutex);
    request_save();
    return id;
}

static void reid_gallery_save_locked(void)
{
    if (ensure_spiffs() != ESP_OK) {
        return;
    }
    FILE *f = std::fopen(REID_GALLERY_PATH, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "failed to open gallery file for writing");
        return;
    }

    gallery_file_header_t header;
    std::memset(&header, 0, sizeof(header));
    header.magic = REID_GALLERY_MAGIC;
    header.version = REID_GALLERY_VERSION;
    header.num_entries = REID_GALLERY_MAX;
    header.feature_dim = REID_FEATURE_DIM;
    header.next_id = s_next_id;
    header.threshold = s_threshold;
    std::fwrite(&header, sizeof(header), 1, f);

    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        gallery_file_entry_t ent;
        std::memset(&ent, 0, sizeof(ent));
        ent.person_id = s_gallery[i].person_id;
        std::memcpy(ent.name, s_gallery[i].name, sizeof(ent.name));
        ent.view = static_cast<uint8_t>(s_gallery[i].view);
        ent.active = s_gallery[i].active ? 1 : 0;
        ent.total_focus_seconds = s_gallery[i].total_focus_seconds;
        ent.hit_count = s_gallery[i].hit_count;
        ent.last_seen_ms = s_gallery[i].last_seen_ms;
        std::memcpy(ent.feature, s_gallery[i].feature, sizeof(ent.feature));
        std::fwrite(&ent, sizeof(ent), 1, f);
    }
    std::fclose(f);
    ESP_LOGI(TAG, "gallery saved to %s", REID_GALLERY_PATH);
}

void reid_gallery_save(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    reid_gallery_save_locked();
    xSemaphoreGive(s_mutex);
}

void reid_gallery_load(void)
{
    if (ensure_spiffs() != ESP_OK) {
        return;
    }
    FILE *f = std::fopen(REID_GALLERY_PATH, "rb");
    if (f == nullptr) {
        ESP_LOGI(TAG, "no existing gallery file, starting empty");
        return;
    }

    gallery_file_header_t header;
    std::memset(&header, 0, sizeof(header));
    if (std::fread(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "gallery file read header failed");
        std::fclose(f);
        return;
    }
    if (header.magic != REID_GALLERY_MAGIC || header.version != REID_GALLERY_VERSION ||
        header.feature_dim != REID_FEATURE_DIM) {
        ESP_LOGE(TAG, "gallery file magic/version/dim mismatch, reset");
        std::fclose(f);
        return;
    }

    s_next_id = header.next_id;
    s_threshold = header.threshold;

    int active = 0;
    for (int i = 0; i < REID_GALLERY_MAX; ++i) {
        gallery_file_entry_t ent;
        std::memset(&ent, 0, sizeof(ent));
        if (std::fread(&ent, sizeof(ent), 1, f) != 1) {
            break;
        }
        s_gallery[i].person_id = ent.person_id;
        std::memcpy(s_gallery[i].name, ent.name, sizeof(s_gallery[i].name));
        s_gallery[i].name[REID_MAX_NAME_LEN] = '\0';
        s_gallery[i].view = static_cast<reid_view_t>(ent.view);
        s_gallery[i].active = (ent.active != 0);
        s_gallery[i].total_focus_seconds = ent.total_focus_seconds;
        s_gallery[i].hit_count = ent.hit_count;
        s_gallery[i].last_seen_ms = ent.last_seen_ms;
        std::memcpy(s_gallery[i].feature, ent.feature, sizeof(s_gallery[i].feature));
        if (s_gallery[i].active) {
            ++active;
        }
    }
    std::fclose(f);
    ESP_LOGI(TAG, "gallery loaded, active=%d", active);
}
