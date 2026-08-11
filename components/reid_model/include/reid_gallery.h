/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "reid_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REID_GALLERY_MAX      8
#define REID_MAX_NAME_LEN     15

typedef enum {
    REID_VIEW_FRONT = 0,
    REID_VIEW_SIDE,
    REID_VIEW_BACK,
    REID_VIEW_UNKNOWN,
} reid_view_t;

typedef struct {
    int         person_id;              /* 0 = inactive */
    char        name[REID_MAX_NAME_LEN + 1];
    reid_view_t view;
    float       feature[REID_FEATURE_DIM];
    uint32_t    total_focus_seconds;    /* cumulative study time */
    uint32_t    last_seen_ms;
    uint32_t    hit_count;
    bool        active;
} reid_person_t;

/**
 * @brief Initialize/load the gallery from SPIFFS.
 */
void reid_gallery_init(void);

/**
 * @brief Reset the in-memory gallery (does not delete SPIFFS file until save).
 */
void reid_gallery_reset(void);

/**
 * @brief Set the matching threshold (default 0.65).
 */
void reid_gallery_set_threshold(float threshold);

/**
 * @brief Query the gallery without creating a new entry.
 *
 * @param feature        L2-normalized 128-d feature.
 * @param threshold      Similarity threshold.
 * @param out_similarity Optional output for the best matching similarity.
 * @return Matched person ID (>0), or -1 if no match above threshold.
 */
int reid_gallery_query(const float *feature, float threshold, float *out_similarity);

/**
 * @brief Compare a feature against a specific person ID.
 *
 * @param person_id      Person ID to compare against.
 * @param feature        L2-normalized 128-d feature.
 * @param out_similarity Output similarity (0 if person not found).
 * @return true if the person exists.
 */
bool reid_gallery_match_person(int person_id, const float *feature, float *out_similarity);

/**
 * @brief EMA-update a specific person's template with a new feature.
 *
 * @param person_id      Person ID to update.
 * @param feature        L2-normalized 128-d feature.
 * @param now_ms         Current timestamp in milliseconds.
 * @param out_similarity Output similarity before update (0 if person not found).
 * @return true if the person exists and was updated.
 */
bool reid_gallery_update_person(int person_id, const float *feature, uint32_t now_ms, float *out_similarity);

/**
 * @brief Compare a new feature against the gallery and return a person ID.
 *
 * If the best match is above the threshold, the existing person is returned.
 * Otherwise a new gallery entry (and new ID) is created.
 *
 * @param feature        L2-normalized 128-d feature.
 * @param now_ms         Current timestamp in milliseconds.
 * @param out_similarity Optional output for the best matching similarity.
 * @return Person ID (>0), or -1 if the gallery is full.
 */
int reid_gallery_update(const float *feature, uint32_t now_ms, float *out_similarity);

/**
 * @brief Find a gallery entry by person ID.
 */
const reid_person_t *reid_gallery_find_by_id(int person_id);

/**
 * @brief Find a gallery entry by name (case-sensitive, exact match).
 * @return Person ID (>0), or -1 if not found.
 */
int reid_gallery_find_by_name(const char *name);

/**
 * @brief Delete a gallery entry by person ID and persist.
 * @return true if the entry was found and deleted.
 */
bool reid_gallery_delete_person(int person_id);

/**
 * @brief Reset the focus-time counter for a person and persist.
 * @return true if the entry was found and updated.
 */
bool reid_gallery_reset_focus_time(int person_id);

/**
 * @brief Add focus seconds to a registered person and persist.
 */
void reid_gallery_add_focus_seconds(int person_id, uint32_t seconds);

/**
 * @brief Return the number of active persons in the gallery.
 */
int reid_gallery_count(void);

/**
 * @brief Get a pointer to an active gallery entry, or NULL if inactive.
 */
const reid_person_t *reid_gallery_get(int idx);

/**
 * @brief Get the name of a person by ID.
 */
const char *reid_gallery_get_name(int person_id);

/**
 * @brief Start enrolling a new person.
 * @return true if a slot was reserved.
 */
bool reid_gallery_enroll_start(const char *name);

/**
 * @brief Feed one feature sample into the pending enrollment.
 * @return true if the sample was accepted.
 */
bool reid_gallery_enroll_feed(const float *feature);

/**
 * @brief Finalize enrollment and return the new person ID.
 * @return Person ID (>0), or -1 on error.
 */
int reid_gallery_enroll_finish(void);

/**
 * @brief Save the gallery to SPIFFS.
 */
void reid_gallery_save(void);

/**
 * @brief Load the gallery from SPIFFS.
 */
void reid_gallery_load(void);

#ifdef __cplusplus
}
#endif
