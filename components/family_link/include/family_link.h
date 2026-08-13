/* Guardian short-voice downlink.  Independent from cloud_sync uplink. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Synchronous playback callback for mono PCM16 at 16 kHz. */
typedef esp_err_t (*family_link_playback_cb_t)(const int16_t *pcm,
                                               size_t sample_count,
                                               void *user_ctx);

/**
 * Start the independent Family Link task. Safe to call once after boot.
 * The callback is invoked without any HTTP/family-link lock being held.
 */
esp_err_t family_link_start(family_link_playback_cb_t playback_cb,
                            void *user_ctx);

#ifdef __cplusplus
}
#endif
