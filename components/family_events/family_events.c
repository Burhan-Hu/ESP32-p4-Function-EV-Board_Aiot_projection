/*
 * Family event state layer. The mutex protects only fixed-size state copies,
 * debounce counters, IDs, and event construction. Callers perform all I/O,
 * Gallery/ReID work, UI, TTS, and cloud queueing after these calls return.
 */
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "family_events.h"
#include "family_utf8.h"

static StaticSemaphore_t s_state_mutex_storage;
static SemaphoreHandle_t s_state_mutex = NULL;
static family_study_session_t s_study_session;
static family_absence_context_t
    s_absence_contexts[FAMILY_MAX_ABSENCE_CONTEXTS];
static char s_device_id[FAMILY_DEVICE_ID_LEN];
static uint64_t s_boot_id = 0;
static uint32_t s_session_sequence = 0;
static uint32_t s_event_sequence = 0;

/* Owner-missing debounce belongs to the one active study session. */
static int32_t s_known_non_owner_candidate = -1;
static uint8_t s_known_non_owner_streak = 0;
static uint8_t s_unknown_owner_streak = 0;
/* False only for generic no-human absence; owner-missing keeps frame flow open. */
static bool s_return_observation_gate = true;

static bool confidence_is_valid(bool valid, float confidence)
{
    return !valid || (confidence >= 0.0f && confidence <= 1.0f);
}

static bool elapsed_ms(uint32_t start_tick_ms, uint32_t end_tick_ms,
                       uint32_t *out_elapsed_ms)
{
    /*
     * Durations in this component are bounded below 2^31 ms. The signed
     * half-range test therefore accepts a legitimate uint32 wrap but rejects
     * a slightly older timestamp that entered the mutex after a newer one.
     */
    int32_t delta = (int32_t)(end_tick_ms - start_tick_ms);
    if (delta < 0 || out_elapsed_ms == NULL) {
        return false;
    }
    *out_elapsed_ms = (uint32_t)delta;
    return true;
}

static bool elapsed_seconds(uint32_t start_tick_ms, uint32_t end_tick_ms,
                            uint32_t *out_elapsed_seconds)
{
    uint32_t duration_ms = 0;
    if (!elapsed_ms(start_tick_ms, end_tick_ms, &duration_ms) ||
        out_elapsed_seconds == NULL) {
        return false;
    }
    *out_elapsed_seconds = duration_ms / 1000U;
    return true;
}

static bool next_sequence(uint32_t *sequence, uint32_t *out_value)
{
    if (sequence == NULL || out_value == NULL || *sequence == UINT32_MAX) {
        return false;
    }
    *sequence += 1U;
    *out_value = *sequence;
    return true;
}

static bool make_scoped_id_locked(char kind, uint32_t *sequence,
                                  char *out, size_t out_len)
{
    uint32_t value = 0;
    if (!next_sequence(sequence, &value)) {
        return false;
    }
    int written = snprintf(out, out_len, "%s-%016" PRIX64 "-%c%08" PRIX32,
                           s_device_id, s_boot_id, kind, value);
    return written > 0 && (size_t)written < out_len;
}

static bool make_event_locked(family_event_type_t type, int64_t timestamp,
                              family_event_t *out_event)
{
    memset(out_event, 0, sizeof(*out_event));
    out_event->schema_version = FAMILY_EVENTS_SCHEMA_VERSION;
    out_event->event_type = type;
    out_event->person_id = -1;
    out_event->timestamp = timestamp;
    strlcpy(out_event->device_id, s_device_id, sizeof(out_event->device_id));
    return make_scoped_id_locked('E', &s_event_sequence,
                                 out_event->event_id,
                                 sizeof(out_event->event_id));
}

static void reset_owner_missing_debounce_locked(void)
{
    s_known_non_owner_candidate = -1;
    s_known_non_owner_streak = 0;
    s_unknown_owner_streak = 0;
}

static family_absence_context_t *find_absence_locked(int32_t person_id)
{
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        if (s_absence_contexts[i].active &&
            s_absence_contexts[i].person_id == person_id) {
            return &s_absence_contexts[i];
        }
    }
    return NULL;
}

static family_absence_context_t *find_free_absence_locked(void)
{
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        if (!s_absence_contexts[i].active) {
            return &s_absence_contexts[i];
        }
    }
    return NULL;
}

static uint32_t expire_stale_absences_locked(uint32_t now_tick_ms)
{
    uint32_t expired = 0;
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        family_absence_context_t *context = &s_absence_contexts[i];
        uint32_t duration_ms = 0;
        if (context->active &&
            elapsed_ms(context->left_tick_ms, now_tick_ms, &duration_ms) &&
            duration_ms >= FAMILY_ABSENCE_TTL_MS) {
            memset(context, 0, sizeof(*context));
            expired++;
        }
    }
    return expired;
}

static void reset_return_streaks_except_locked(int32_t person_id,
                                               uint32_t observation_tick_ms)
{
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        family_absence_context_t *context = &s_absence_contexts[i];
        uint32_t ignored = 0;
        if (context->active && context->person_id != person_id &&
            elapsed_ms(context->left_tick_ms, observation_tick_ms, &ignored)) {
            context->return_match_streak = 0;
        }
    }
}

static const char *absence_source_detail(family_absence_source_t source)
{
    switch (source) {
    case FAMILY_ABSENCE_SOURCE_HUMAN_LEFT:
        return "human_left";
    case FAMILY_ABSENCE_SOURCE_KNOWN_NON_OWNER:
        return "owner_missing_known_non_owner";
    case FAMILY_ABSENCE_SOURCE_UNKNOWN_OWNER_MISSING:
        return "owner_missing_unknown";
    default:
        return "owner_missing";
    }
}

static bool build_focus_end_locked(const family_study_session_t *session,
                                   uint32_t end_tick_ms, int64_t end_epoch,
                                   family_focus_end_reason_t reason,
                                   const char *detail,
                                   family_event_t *out_event)
{
    uint32_t focus_seconds = 0;
    if (!elapsed_seconds(session->start_tick_ms, end_tick_ms,
                         &focus_seconds)) {
        return false;
    }
    family_event_t event;
    if (!make_event_locked(FAMILY_EVENT_FOCUS_END, end_epoch, &event)) {
        return false;
    }
    event.person_id = session->person_id;
    strlcpy(event.person_name, session->person_name,
            sizeof(event.person_name));
    strlcpy(event.session_id, session->session_id,
            sizeof(event.session_id));
    event.focus_seconds = focus_seconds;
    event.confidence_valid = session->start_reid_similarity_valid;
    event.confidence = session->start_reid_similarity;
    event.end_reason_valid = true;
    event.end_reason = reason;
    strlcpy(event.detail, detail ? detail : family_focus_end_reason_name(reason),
            sizeof(event.detail));
    *out_event = event;
    return true;
}

/* Caller holds s_state_mutex. No external module is called here. */
static bool transition_to_absence_locked(uint32_t left_tick_ms,
                                         int64_t left_epoch,
                                         family_absence_source_t source,
                                         family_event_t *out_focus_end)
{
    if (!s_study_session.active ||
        find_absence_locked(s_study_session.person_id) != NULL) {
        return false;
    }

    uint32_t focus_seconds = 0;
    if (!elapsed_seconds(s_study_session.start_tick_ms, left_tick_ms,
                         &focus_seconds)) {
        return false;
    }

    family_absence_context_t *slot = find_free_absence_locked();
    if (slot == NULL) {
        return false;
    }

    family_event_t end_event;
    if (!build_focus_end_locked(&s_study_session, left_tick_ms, left_epoch,
                                FAMILY_FOCUS_END_HUMAN_LEFT,
                                absence_source_detail(source), &end_event)) {
        return false;
    }

    family_absence_context_t absence = {0};
    absence.active = true;
    strlcpy(absence.session_id, s_study_session.session_id,
            sizeof(absence.session_id));
    absence.person_id = s_study_session.person_id;
    strlcpy(absence.person_name, s_study_session.person_name,
            sizeof(absence.person_name));
    absence.left_tick_ms = left_tick_ms;
    absence.left_epoch = left_epoch;
    absence.focus_seconds_at_leave = focus_seconds;

    /* Commit all state only after slot and event construction succeeded. */
    *slot = absence;
    if (source == FAMILY_ABSENCE_SOURCE_HUMAN_LEFT) {
        s_return_observation_gate = false;
    }
    memset(&s_study_session, 0, sizeof(s_study_session));
    reset_owner_missing_debounce_locked();
    *out_focus_end = end_event;
    return true;
}

static bool build_seat_return_locked(family_absence_context_t *context,
                                     bool similarity_valid, float similarity,
                                     uint32_t now_tick_ms, int64_t now_epoch,
                                     family_event_t *out_event)
{
    uint32_t absence_seconds = 0;
    if (!elapsed_seconds(context->left_tick_ms, now_tick_ms,
                         &absence_seconds)) {
        return false;
    }
    family_event_t event;
    if (!make_event_locked(FAMILY_EVENT_SEAT_RETURN, now_epoch, &event)) {
        return false;
    }
    event.person_id = context->person_id;
    strlcpy(event.person_name, context->person_name,
            sizeof(event.person_name));
    strlcpy(event.session_id, context->session_id,
            sizeof(event.session_id));
    event.focus_seconds = context->focus_seconds_at_leave;
    event.absence_seconds = absence_seconds;
    event.confidence_valid = similarity_valid;
    event.confidence = similarity;
    strlcpy(event.detail, "seat_return_confirmed", sizeof(event.detail));
    *out_event = event;
    return true;
}

static bool build_long_absence_locked(family_absence_context_t *context,
                                      uint32_t now_tick_ms, int64_t now_epoch,
                                      family_event_t *out_event)
{
    uint32_t absence_seconds = 0;
    if (!elapsed_seconds(context->left_tick_ms, now_tick_ms,
                         &absence_seconds)) {
        return false;
    }
    family_event_t event;
    if (!make_event_locked(FAMILY_EVENT_LONG_ABSENCE, now_epoch, &event)) {
        return false;
    }
    event.person_id = context->person_id;
    strlcpy(event.person_name, context->person_name,
            sizeof(event.person_name));
    strlcpy(event.session_id, context->session_id,
            sizeof(event.session_id));
    event.focus_seconds = context->focus_seconds_at_leave;
    event.absence_seconds = absence_seconds;
    strlcpy(event.detail, "long_absence", sizeof(event.detail));
    *out_event = event;
    return true;
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

esp_err_t family_events_init(void)
{
    if (s_state_mutex != NULL) {
        return ESP_OK;
    }

    esp_err_t err = resolve_device_id();
    if (err != ESP_OK) {
        return err;
    }

    s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_storage);
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_study_session, 0, sizeof(s_study_session));
    memset(s_absence_contexts, 0, sizeof(s_absence_contexts));
    reset_owner_missing_debounce_locked();
    s_return_observation_gate = true;
    s_boot_id = ((uint64_t)esp_random() << 32) | (uint64_t)esp_random();
    if (s_boot_id == 0) {
        s_boot_id = 1;
    }
    s_session_sequence = 0;
    s_event_sequence = 0;
    return ESP_OK;
}

const char *family_event_type_name(family_event_type_t type)
{
    switch (type) {
    case FAMILY_EVENT_FOCUS_START:
        return "FOCUS_START";
    case FAMILY_EVENT_FOCUS_END:
        return "FOCUS_END";
    case FAMILY_EVENT_LONG_ABSENCE:
        return "LONG_ABSENCE";
    case FAMILY_EVENT_SEAT_RETURN:
        return "SEAT_RETURN";
    case FAMILY_EVENT_GUARD_ALERT:
        return "GUARD_ALERT";
    default:
        return "UNKNOWN";
    }
}

const char *family_focus_end_reason_name(family_focus_end_reason_t reason)
{
    switch (reason) {
    case FAMILY_FOCUS_END_USER_STOP:
        return "USER_STOP";
    case FAMILY_FOCUS_END_HUMAN_LEFT:
        return "HUMAN_LEFT";
    case FAMILY_FOCUS_END_OTHER:
        return "OTHER";
    default:
        return "UNKNOWN";
    }
}

bool family_event_validate(const family_event_t *event)
{
    if (event == NULL || event->schema_version != FAMILY_EVENTS_SCHEMA_VERSION ||
        event->event_type < FAMILY_EVENT_FOCUS_START ||
        event->event_type > FAMILY_EVENT_GUARD_ALERT ||
        !family_utf8_field_is_valid(event->event_id,
                                    sizeof(event->event_id)) ||
        event->event_id[0] == '\0' ||
        !family_utf8_field_is_valid(event->device_id,
                                    sizeof(event->device_id)) ||
        event->device_id[0] == '\0' ||
        !family_utf8_field_is_valid(event->person_name,
                                    sizeof(event->person_name)) ||
        !family_utf8_field_is_valid(event->session_id,
                                    sizeof(event->session_id)) ||
        !family_utf8_field_is_valid(event->detail, sizeof(event->detail)) ||
        !confidence_is_valid(event->confidence_valid, event->confidence)) {
        return false;
    }

    if (event->event_type != FAMILY_EVENT_GUARD_ALERT &&
        (event->person_id <= 0 || event->person_name[0] == '\0' ||
         event->session_id[0] == '\0')) {
        return false;
    }
    if (event->event_type == FAMILY_EVENT_FOCUS_END) {
        if (!event->end_reason_valid ||
            event->end_reason < FAMILY_FOCUS_END_USER_STOP ||
            event->end_reason > FAMILY_FOCUS_END_OTHER) {
            return false;
        }
    } else if (event->end_reason_valid) {
        return false;
    }
    return true;
}

family_session_start_result_t family_events_start_session(
    int32_t person_id, const char *person_name,
    uint32_t start_tick_ms, int64_t start_epoch,
    bool similarity_valid, float similarity,
    family_event_t *out_event,
    family_session_guard_t *out_conflicting_session)
{
    char safe_person_name[FAMILY_PERSON_NAME_LEN];
    if (s_state_mutex == NULL || out_event == NULL || person_id <= 0 ||
        person_name == NULL || person_name[0] == '\0' ||
        !confidence_is_valid(similarity_valid, similarity) ||
        !family_utf8_copy_truncate(safe_person_name,
                                   sizeof(safe_person_name), person_name) ||
        safe_person_name[0] == '\0') {
        return FAMILY_SESSION_START_INVALID;
    }

    memset(out_event, 0, sizeof(*out_event));
    if (out_conflicting_session != NULL) {
        memset(out_conflicting_session, 0, sizeof(*out_conflicting_session));
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    expire_stale_absences_locked(start_tick_ms);

    /* Do not stop a different active learner merely because the requested
     * learner has only one (still unconfirmed) return observation. */
    if (find_absence_locked(person_id) != NULL) {
        xSemaphoreGive(s_state_mutex);
        return FAMILY_SESSION_START_RETURN_CONFIRM_PENDING;
    }
    if (s_study_session.active) {
        family_session_start_result_t result =
            s_study_session.person_id == person_id
                ? FAMILY_SESSION_START_ALREADY_ACTIVE
                : FAMILY_SESSION_START_OTHER_ACTIVE;
        if (result == FAMILY_SESSION_START_OTHER_ACTIVE &&
            out_conflicting_session != NULL) {
            out_conflicting_session->active = true;
            strlcpy(out_conflicting_session->session_id,
                    s_study_session.session_id,
                    sizeof(out_conflicting_session->session_id));
        }
        xSemaphoreGive(s_state_mutex);
        return result;
    }

    char session_id[FAMILY_SESSION_ID_LEN] = {0};
    family_event_t event;
    bool ok = make_scoped_id_locked('S', &s_session_sequence,
                                    session_id, sizeof(session_id)) &&
              make_event_locked(FAMILY_EVENT_FOCUS_START, start_epoch, &event);
    if (ok) {
        family_study_session_t session = {0};
        session.active = true;
        strlcpy(session.session_id, session_id, sizeof(session.session_id));
        session.person_id = person_id;
        family_utf8_copy_truncate(session.person_name,
                                  sizeof(session.person_name), safe_person_name);
        session.start_tick_ms = start_tick_ms;
        session.start_epoch = start_epoch;
        session.start_reid_similarity_valid = similarity_valid;
        session.start_reid_similarity = similarity;

        event.person_id = person_id;
        family_utf8_copy_truncate(event.person_name,
                                  sizeof(event.person_name), safe_person_name);
        strlcpy(event.session_id, session_id, sizeof(event.session_id));
        event.confidence_valid = similarity_valid;
        event.confidence = similarity;
        strlcpy(event.detail, "focus_started", sizeof(event.detail));

        s_study_session = session;
        reset_owner_missing_debounce_locked();
        *out_event = event;
    }
    xSemaphoreGive(s_state_mutex);
    return ok ? FAMILY_SESSION_START_OK : FAMILY_SESSION_START_INVALID;
}

bool family_events_get_study_session(family_study_session_t *out_session)
{
    if (s_state_mutex == NULL || out_session == NULL) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool active = s_study_session.active;
    if (active) {
        *out_session = s_study_session;
    } else {
        memset(out_session, 0, sizeof(*out_session));
    }
    xSemaphoreGive(s_state_mutex);
    return active;
}

bool family_events_get_session_guard(family_session_guard_t *out_guard)
{
    if (s_state_mutex == NULL || out_guard == NULL) {
        return false;
    }
    memset(out_guard, 0, sizeof(*out_guard));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_study_session.active) {
        out_guard->active = true;
        strlcpy(out_guard->session_id, s_study_session.session_id,
                sizeof(out_guard->session_id));
    }
    xSemaphoreGive(s_state_mutex);
    return out_guard->active;
}

static bool session_guard_matches_locked(
    const family_session_guard_t *expected_session)
{
    return expected_session != NULL && expected_session->active &&
           expected_session->session_id[0] != '\0' &&
           s_study_session.active &&
           strcmp(expected_session->session_id,
                  s_study_session.session_id) == 0;
}

bool family_events_end_session(const family_session_guard_t *expected_session,
                               uint32_t end_tick_ms, int64_t end_epoch,
                               family_focus_end_reason_t reason,
                               family_event_t *out_event)
{
    if (s_state_mutex == NULL || out_event == NULL ||
        reason < FAMILY_FOCUS_END_USER_STOP || reason > FAMILY_FOCUS_END_OTHER) {
        return false;
    }

    memset(out_event, 0, sizeof(*out_event));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!session_guard_matches_locked(expected_session)) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }

    family_event_t event;
    bool ok = build_focus_end_locked(&s_study_session, end_tick_ms, end_epoch,
                                     reason, NULL, &event);
    if (ok) {
        *out_event = event;
        memset(&s_study_session, 0, sizeof(s_study_session));
        reset_owner_missing_debounce_locked();
    }
    xSemaphoreGive(s_state_mutex);
    return ok;
}

bool family_events_end_and_begin_absence(
    const family_session_guard_t *expected_session,
    uint32_t left_tick_ms, int64_t left_epoch,
    family_absence_source_t source, family_event_t *out_focus_end)
{
    if (s_state_mutex == NULL || out_focus_end == NULL ||
        source < FAMILY_ABSENCE_SOURCE_HUMAN_LEFT ||
        source > FAMILY_ABSENCE_SOURCE_UNKNOWN_OWNER_MISSING) {
        return false;
    }
    memset(out_focus_end, 0, sizeof(*out_focus_end));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!session_guard_matches_locked(expected_session)) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }
    expire_stale_absences_locked(left_tick_ms);
    bool ok = transition_to_absence_locked(left_tick_ms, left_epoch, source,
                                           out_focus_end);
    xSemaphoreGive(s_state_mutex);
    return ok;
}

void family_events_mark_human_present(void)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_return_observation_gate = true;
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        if (s_absence_contexts[i].active) {
            s_absence_contexts[i].return_match_streak = 0;
        }
    }
    xSemaphoreGive(s_state_mutex);
}

bool family_events_observe_owner(
    family_owner_observation_kind_t kind, int32_t person_id,
    bool similarity_valid, float similarity,
    uint32_t now_tick_ms, int64_t now_epoch,
    family_owner_observation_result_t *out_result)
{
    if (s_state_mutex == NULL || out_result == NULL ||
        kind < FAMILY_OWNER_OBSERVATION_NO_PERSON ||
        kind > FAMILY_OWNER_OBSERVATION_KNOWN ||
        !confidence_is_valid(similarity_valid, similarity)) {
        return false;
    }

    memset(out_result, 0, sizeof(*out_result));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    expire_stale_absences_locked(now_tick_ms);

    uint32_t session_observation_age = 0;
    bool observation_can_affect_session =
        !s_study_session.active ||
        elapsed_ms(s_study_session.start_tick_ms, now_tick_ms,
                   &session_observation_age);
    (void)session_observation_age;

    /* A known identity requires both a positive ID and a valid ReID score. */
    if (kind == FAMILY_OWNER_OBSERVATION_KNOWN &&
        (person_id <= 0 || !similarity_valid)) {
        kind = FAMILY_OWNER_OBSERVATION_UNKNOWN;
        person_id = -1;
    }

    if (kind == FAMILY_OWNER_OBSERVATION_KNOWN) {
        if (s_study_session.active && observation_can_affect_session &&
            person_id == s_study_session.person_id) {
            reset_owner_missing_debounce_locked();
        } else if (s_study_session.active && observation_can_affect_session) {
            s_unknown_owner_streak = 0;
            if (s_known_non_owner_candidate == person_id) {
                if (s_known_non_owner_streak < UINT8_MAX) {
                    s_known_non_owner_streak++;
                }
            } else {
                s_known_non_owner_candidate = person_id;
                s_known_non_owner_streak = 1;
            }
            if (s_known_non_owner_streak >=
                FAMILY_OWNER_KNOWN_MISSING_MATCHES) {
                out_result->focus_end_valid = transition_to_absence_locked(
                    now_tick_ms, now_epoch,
                    FAMILY_ABSENCE_SOURCE_KNOWN_NON_OWNER,
                    &out_result->focus_end);
            }
        } else {
            reset_owner_missing_debounce_locked();
        }

        reset_return_streaks_except_locked(person_id, now_tick_ms);
        family_absence_context_t *return_context = s_return_observation_gate
            ? find_absence_locked(person_id) : NULL;
        if (return_context != NULL) {
            uint32_t absence_elapsed_ms = 0;
            if (!elapsed_ms(return_context->left_tick_ms, now_tick_ms,
                            &absence_elapsed_ms)) {
                xSemaphoreGive(s_state_mutex);
                return out_result->focus_end_valid;
            }
            if (return_context->return_match_streak <
                FAMILY_RETURN_CONFIRM_MATCHES) {
                return_context->return_match_streak++;
            }
            if (return_context->return_match_streak >=
                FAMILY_RETURN_CONFIRM_MATCHES) {
                /* If the return races the 1 s maintainer at the exact LONG
                 * threshold, materialize LONG first so closing the context
                 * cannot erase an already-due fact. */
                if (!return_context->long_absence_emitted &&
                    absence_elapsed_ms >= FAMILY_LONG_ABSENCE_MS) {
                    family_event_t long_event;
                    if (!build_long_absence_locked(return_context,
                                                   now_tick_ms, now_epoch,
                                                   &long_event)) {
                        xSemaphoreGive(s_state_mutex);
                        return out_result->focus_end_valid;
                    }
                    return_context->long_absence_emitted = true;
                    strlcpy(return_context->long_absence_event_id,
                            long_event.event_id,
                            sizeof(return_context->long_absence_event_id));
                    out_result->long_absence_valid = true;
                    out_result->long_absence = long_event;
                }
                family_event_t return_event;
                if (build_seat_return_locked(return_context,
                                             similarity_valid, similarity,
                                             now_tick_ms, now_epoch,
                                             &return_event)) {
                    out_result->seat_return_valid = true;
                    out_result->seat_return = return_event;
                    memset(return_context, 0, sizeof(*return_context));
                }
            }
        }
    } else if (kind == FAMILY_OWNER_OBSERVATION_UNKNOWN) {
        s_known_non_owner_candidate = -1;
        s_known_non_owner_streak = 0;
        if (s_study_session.active && observation_can_affect_session) {
            if (s_unknown_owner_streak < UINT8_MAX) {
                s_unknown_owner_streak++;
            }
            if (s_unknown_owner_streak >=
                FAMILY_OWNER_UNKNOWN_MISSING_MATCHES) {
                out_result->focus_end_valid = transition_to_absence_locked(
                    now_tick_ms, now_epoch,
                    FAMILY_ABSENCE_SOURCE_UNKNOWN_OWNER_MISSING,
                    &out_result->focus_end);
            }
        } else {
            s_unknown_owner_streak = 0;
        }
        reset_return_streaks_except_locked(-1, now_tick_ms);
    } else {
        /* Generic no-human debounce and transition stay in human_detect. */
        reset_owner_missing_debounce_locked();
        reset_return_streaks_except_locked(-1, now_tick_ms);
    }

    bool changed = out_result->focus_end_valid ||
                   out_result->long_absence_valid ||
                   out_result->seat_return_valid;
    xSemaphoreGive(s_state_mutex);
    return changed;
}

bool family_events_get_absence_context(int32_t person_id,
                                       family_absence_context_t *out_context)
{
    if (s_state_mutex == NULL || out_context == NULL || person_id <= 0) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    family_absence_context_t *context = find_absence_locked(person_id);
    if (context != NULL) {
        *out_context = *context;
    } else {
        memset(out_context, 0, sizeof(*out_context));
    }
    xSemaphoreGive(s_state_mutex);
    return context != NULL;
}

size_t family_events_get_absence_count(void)
{
    if (s_state_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        if (s_absence_contexts[i].active) {
            count++;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}

size_t family_events_maintain(uint32_t now_tick_ms, int64_t now_epoch,
                              family_event_t *out_events, size_t capacity,
                              uint32_t *out_expired_count)
{
    if (out_expired_count != NULL) {
        *out_expired_count = 0;
    }
    if (s_state_mutex == NULL || (capacity > 0 && out_events == NULL)) {
        return 0;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint32_t expired = expire_stale_absences_locked(now_tick_ms);
    size_t count = 0;
    for (size_t i = 0;
         i < FAMILY_MAX_ABSENCE_CONTEXTS && count < capacity; ++i) {
        family_absence_context_t *context = &s_absence_contexts[i];
        uint32_t duration_ms = 0;
        if (!context->active || context->long_absence_emitted ||
            !elapsed_ms(context->left_tick_ms, now_tick_ms, &duration_ms) ||
            duration_ms < FAMILY_LONG_ABSENCE_MS) {
            continue;
        }

        family_event_t event;
        if (!build_long_absence_locked(context, now_tick_ms, now_epoch,
                                       &event)) {
            continue;
        }

        context->long_absence_emitted = true;
        strlcpy(context->long_absence_event_id, event.event_id,
                sizeof(context->long_absence_event_id));
        out_events[count++] = event;
    }
    if (out_expired_count != NULL) {
        *out_expired_count = expired;
    }
    xSemaphoreGive(s_state_mutex);
    return count;
}

static bool invalidate_locked(int32_t person_id, bool all,
                              uint32_t now_tick_ms, int64_t now_epoch,
                              family_invalidation_result_t *out_result)
{
    family_invalidation_result_t result = {0};
    bool session_matches = s_study_session.active &&
                           (all || s_study_session.person_id == person_id);
    if (session_matches) {
        if (!build_focus_end_locked(&s_study_session, now_tick_ms, now_epoch,
                                    FAMILY_FOCUS_END_OTHER,
                                    all ? "gallery_reset" : "person_deleted",
                                    &result.focus_end)) {
            return false;
        }
        result.focus_end_valid = true;
    }

    if (session_matches) {
        memset(&s_study_session, 0, sizeof(s_study_session));
        reset_owner_missing_debounce_locked();
    }
    if (all) {
        s_return_observation_gate = true;
    }
    for (size_t i = 0; i < FAMILY_MAX_ABSENCE_CONTEXTS; ++i) {
        family_absence_context_t *context = &s_absence_contexts[i];
        if (context->active && (all || context->person_id == person_id)) {
            memset(context, 0, sizeof(*context));
            result.absences_invalidated++;
        }
    }
    *out_result = result;
    return true;
}

bool family_events_invalidate_person(int32_t person_id,
                                     uint32_t now_tick_ms, int64_t now_epoch,
                                     family_invalidation_result_t *out_result)
{
    if (s_state_mutex == NULL || out_result == NULL || person_id <= 0) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool ok = invalidate_locked(person_id, false, now_tick_ms, now_epoch,
                                out_result);
    xSemaphoreGive(s_state_mutex);
    return ok;
}

bool family_events_invalidate_all(uint32_t now_tick_ms, int64_t now_epoch,
                                  family_invalidation_result_t *out_result)
{
    if (s_state_mutex == NULL || out_result == NULL) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool ok = invalidate_locked(-1, true, now_tick_ms, now_epoch, out_result);
    xSemaphoreGive(s_state_mutex);
    return ok;
}

bool family_events_make_guard_alert(int32_t person_id, const char *person_name,
                                    int64_t timestamp,
                                    bool confidence_valid, float confidence,
                                    const char *detail,
                                    family_event_t *out_event)
{
    char safe_person_name[FAMILY_PERSON_NAME_LEN] = {0};
    char safe_detail[FAMILY_EVENT_DETAIL_LEN];
    if (s_state_mutex == NULL || out_event == NULL ||
        !confidence_is_valid(confidence_valid, confidence) ||
        !family_utf8_copy_truncate(safe_detail, sizeof(safe_detail),
                                   detail ? detail : "") ||
        (person_id > 0 &&
         !family_utf8_copy_truncate(safe_person_name,
                                    sizeof(safe_person_name),
                                    person_name ? person_name : ""))) {
        return false;
    }
    memset(out_event, 0, sizeof(*out_event));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    family_event_t event;
    bool ok = make_event_locked(FAMILY_EVENT_GUARD_ALERT, timestamp, &event);
    if (ok) {
        if (person_id > 0) {
            event.person_id = person_id;
            family_utf8_copy_truncate(event.person_name,
                                      sizeof(event.person_name),
                                      safe_person_name);
        }
        event.confidence_valid = confidence_valid;
        event.confidence = confidence;
        family_utf8_copy_truncate(event.detail, sizeof(event.detail),
                                  safe_detail);
        *out_event = event;
    }
    xSemaphoreGive(s_state_mutex);
    return ok;
}
