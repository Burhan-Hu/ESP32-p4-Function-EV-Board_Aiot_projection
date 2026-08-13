/*
 * Family event state: immutable study ownership, owner-presence debounce,
 * bounded absence contexts, and fact-event construction. No I/O is performed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAMILY_EVENTS_SCHEMA_VERSION              1U
#define FAMILY_DEVICE_ID_LEN                      24
#define FAMILY_EVENT_ID_LEN                       48
#define FAMILY_SESSION_ID_LEN                     48
#define FAMILY_PERSON_NAME_LEN                    16
#define FAMILY_EVENT_DETAIL_LEN                   128

/*
 * The detector runs about once every 3 seconds. A known non-owner is strong
 * evidence, so two consecutive observations (normally completed within about
 * 3-6 s from the real change) end the owner's session. Unknown is weaker and
 * requires three consecutive observations (normally about 6-9 s).
 * A return likewise needs two independent, consecutive ReID observations.
 */
#define FAMILY_OWNER_KNOWN_MISSING_MATCHES        2U
#define FAMILY_OWNER_UNKNOWN_MISSING_MATCHES      3U
#define FAMILY_RETURN_CONFIRM_MATCHES             2U

/*
 * LONG_ABSENCE is a family fact threshold. TTL is only an internal stale-state
 * safety ceiling and deliberately does not create a notification event.
 * 24 hours remains safely below one half of the uint32 millisecond range.
 */
#define FAMILY_LONG_ABSENCE_MS                    60000U
#define FAMILY_ABSENCE_TTL_MS                     86400000U

/* Current Gallery capacity is eight people; contexts are bounded likewise. */
#define FAMILY_MAX_ABSENCE_CONTEXTS               8U

#if FAMILY_ABSENCE_TTL_MS <= FAMILY_LONG_ABSENCE_MS
#error "FAMILY_ABSENCE_TTL_MS must exceed FAMILY_LONG_ABSENCE_MS"
#endif

typedef enum {
    FAMILY_EVENT_FOCUS_START = 0,
    FAMILY_EVENT_FOCUS_END,
    FAMILY_EVENT_LONG_ABSENCE,
    FAMILY_EVENT_SEAT_RETURN,
    FAMILY_EVENT_GUARD_ALERT,
} family_event_type_t;

typedef enum {
    FAMILY_FOCUS_END_USER_STOP = 0,
    FAMILY_FOCUS_END_HUMAN_LEFT,
    FAMILY_FOCUS_END_OTHER,
} family_focus_end_reason_t;

typedef enum {
    FAMILY_OWNER_OBSERVATION_NO_PERSON = 0,
    FAMILY_OWNER_OBSERVATION_UNKNOWN,
    FAMILY_OWNER_OBSERVATION_KNOWN,
} family_owner_observation_kind_t;

typedef enum {
    FAMILY_ABSENCE_SOURCE_HUMAN_LEFT = 0,
    FAMILY_ABSENCE_SOURCE_KNOWN_NON_OWNER,
    FAMILY_ABSENCE_SOURCE_UNKNOWN_OWNER_MISSING,
} family_absence_source_t;

typedef enum {
    FAMILY_SESSION_START_OK = 0,
    FAMILY_SESSION_START_ALREADY_ACTIVE,
    FAMILY_SESSION_START_OTHER_ACTIVE,
    FAMILY_SESSION_START_RETURN_CONFIRM_PENDING,
    FAMILY_SESSION_START_INVALID,
} family_session_start_result_t;

typedef struct {
    uint32_t schema_version;
    char event_id[FAMILY_EVENT_ID_LEN];
    char device_id[FAMILY_DEVICE_ID_LEN];
    family_event_type_t event_type;
    int32_t person_id;
    char person_name[FAMILY_PERSON_NAME_LEN];
    char session_id[FAMILY_SESSION_ID_LEN];
    int64_t timestamp;
    uint32_t focus_seconds;
    uint32_t absence_seconds;
    bool confidence_valid;
    float confidence;
    bool end_reason_valid;
    family_focus_end_reason_t end_reason;
    char detail[FAMILY_EVENT_DETAIL_LEN];
} family_event_t;

typedef struct {
    bool active;
    char session_id[FAMILY_SESSION_ID_LEN];
    int32_t person_id;
    char person_name[FAMILY_PERSON_NAME_LEN];
    uint32_t start_tick_ms;
    int64_t start_epoch;
    bool start_reid_similarity_valid;
    float start_reid_similarity;
} family_study_session_t;

typedef struct {
    bool active;
    char session_id[FAMILY_SESSION_ID_LEN];
    int32_t person_id;
    char person_name[FAMILY_PERSON_NAME_LEN];
    uint32_t left_tick_ms;
    int64_t left_epoch;
    uint32_t focus_seconds_at_leave;
    bool long_absence_emitted;
    uint8_t return_match_streak;
    char long_absence_event_id[FAMILY_EVENT_ID_LEN];
} family_absence_context_t;

typedef struct {
    bool active;
    char session_id[FAMILY_SESSION_ID_LEN];
} family_session_guard_t;

/* Facts produced by one already-computed owner observation. */
typedef struct {
    bool focus_end_valid;
    family_event_t focus_end;
    bool long_absence_valid;
    family_event_t long_absence;
    bool seat_return_valid;
    family_event_t seat_return;
} family_owner_observation_result_t;

typedef struct {
    bool focus_end_valid;
    family_event_t focus_end;
    uint32_t absences_invalidated;
} family_invalidation_result_t;

/** Initialize the state mutex and this-boot ID namespace. Safe sequentially. */
esp_err_t family_events_init(void);

/** Stable protocol strings for JSON/logging; returns "UNKNOWN" if invalid. */
const char *family_event_type_name(family_event_type_t type);
const char *family_focus_end_reason_name(family_focus_end_reason_t reason);

/** Validate a fully constructed event before handing it to an I/O layer. */
bool family_event_validate(const family_event_t *event);

/**
 * Start a session without disturbing other people's absence contexts.
 * A still-open absence for the same person returns RETURN_CONFIRM_PENDING;
 * only ReID observations may close that absence.
 */
family_session_start_result_t family_events_start_session(
    int32_t person_id, const char *person_name,
    uint32_t start_tick_ms, int64_t start_epoch,
    bool similarity_valid, float similarity,
    family_event_t *out_event,
    family_session_guard_t *out_conflicting_session);

/** Snapshot the current study session. */
bool family_events_get_study_session(family_study_session_t *out_session);

/** Snapshot the active session token used to reject stale callback intents. */
bool family_events_get_session_guard(family_session_guard_t *out_guard);

/** Atomically construct FOCUS_END and close a session without making absence. */
bool family_events_end_session(const family_session_guard_t *expected_session,
                               uint32_t end_tick_ms, int64_t end_epoch,
                               family_focus_end_reason_t reason,
                               family_event_t *out_event);

/**
 * Atomically snapshot active session -> absence, construct its FOCUS_END, and
 * close that session in one family mutex critical section.
 */
bool family_events_end_and_begin_absence(
    const family_session_guard_t *expected_session,
    uint32_t left_tick_ms, int64_t left_epoch,
    family_absence_source_t source, family_event_t *out_focus_end);

/** Open return verification after the generic absent->present edge. */
void family_events_mark_human_present(void);

/**
 * Consume exactly one already-computed detector/ReID observation. This never
 * runs ReID. Stable non-owner/unknown evidence can atomically create absence;
 * two matching observations close only that matching person's absence.
 */
bool family_events_observe_owner(
    family_owner_observation_kind_t kind, int32_t person_id,
    bool similarity_valid, float similarity,
    uint32_t now_tick_ms, int64_t now_epoch,
    family_owner_observation_result_t *out_result);

/** Snapshot one person's current absence context. */
bool family_events_get_absence_context(int32_t person_id,
                                       family_absence_context_t *out_context);

/** Number of active absence contexts, for diagnostics/tests. */
size_t family_events_get_absence_count(void);

/**
 * Materialize due LONG_ABSENCE facts and expire contexts at the internal TTL.
 * Only facts actually copied to out_events are marked emitted; a small caller
 * capacity is safe because remaining due contexts are retried next tick.
 */
size_t family_events_maintain(uint32_t now_tick_ms, int64_t now_epoch,
                              family_event_t *out_events, size_t capacity,
                              uint32_t *out_expired_count);

/** Atomically invalidate a person (or all state) for Gallery delete/reset. */
bool family_events_invalidate_person(int32_t person_id,
                                     uint32_t now_tick_ms, int64_t now_epoch,
                                     family_invalidation_result_t *out_result);
bool family_events_invalidate_all(uint32_t now_tick_ms, int64_t now_epoch,
                                  family_invalidation_result_t *out_result);

/** Construct a GUARD_ALERT fact without changing PIR/VLM state. */
bool family_events_make_guard_alert(int32_t person_id, const char *person_name,
                                    int64_t timestamp,
                                    bool confidence_valid, float confidence,
                                    const char *detail,
                                    family_event_t *out_event);

#ifdef __cplusplus
}
#endif
