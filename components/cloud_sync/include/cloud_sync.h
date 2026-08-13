/*
 * Cloud sync: report focus sessions and safety alerts to the configured
 * Guardian server over HTTP(S). All reporting is asynchronous (queue + worker task);
 * failures are counted/logged and never block the main pipeline.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "family_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Alert type for the leave-seat reminder. */
#define CLOUD_SYNC_ALERT_LEAVE_SEAT  "leave_seat"
/** Alert type for guard-mode (monitoring) anomalies. */
#define CLOUD_SYNC_ALERT_GUARD       "guard_alert"

/**
 * @brief Start the cloud sync worker (queue, HTTP task, SNTP time sync).
 *
 * Call once after Wi-Fi is connected. Safe to call when the server is
 * unreachable: events will simply be dropped on timeout.
 */
esp_err_t cloud_sync_init(void);

/**
 * @brief Non-blocking publish of one complete family fact event.
 *
 * The event is validated and copied by value into the FreeRTOS queue. This
 * function never performs HTTP and uses a zero queue wait. It returns false
 * when cloud sync is disabled/uninitialized, the event is invalid, or the
 * queue is full.
 */
bool cloud_sync_publish(const family_event_t *event);

typedef struct {
    uint32_t enqueued_events;
    uint32_t disabled_drops;
    uint32_t queue_full_drops;
    uint32_t invalid_event_drops;
} cloud_sync_stats_t;

/** Copy current enqueue/drop counters. */
void cloud_sync_get_stats(cloud_sync_stats_t *out_stats);

/**
 * @brief Report that a focus session has started.
 *
 * @param name        registered person name (UTF-8)
 * @param start_epoch session start time (time(NULL)); pass 0 to use now
 */
void cloud_sync_focus_start(const char *name, time_t start_epoch);

/**
 * @brief Report that a focus session has ended.
 *
 * @param name         registered person name (UTF-8)
 * @param start_epoch  session start time previously captured
 * @param end_epoch    session end time (time(NULL)); pass 0 to use now
 * @param duration_sec session duration in seconds
 */
void cloud_sync_focus_end(const char *name, time_t start_epoch,
                          time_t end_epoch, uint32_t duration_sec);

/**
 * @brief Report a safety alert (leave-seat / guard anomaly).
 *
 * @param type        CLOUD_SYNC_ALERT_LEAVE_SEAT or CLOUD_SYNC_ALERT_GUARD
 * @param confidence  0-100
 * @param detail      short UTF-8 description (may be truncated)
 */
void cloud_sync_alert(const char *type, int confidence, const char *detail);

#ifdef __cplusplus
}
#endif
