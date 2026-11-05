/* time_sync.h - wall-clock time for Sparkplug timestamps.
 *
 * Sparkplug timestamps are milliseconds since the Unix epoch (UTC). The
 * ESP32 has no battery-backed clock, so we ask an SNTP server once and keep
 * an offset to the kernel uptime counter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Blocking SNTP query (a few retries). Needs the network to be up. */
int time_sync_initialize(void);

/* Re-syncs if never synced or the last sync is older than
 * APP_SNTP_RESYNC_S. Cheap to call often; only talks to the network
 * when needed. */
void time_sync_refresh_if_stale(void);

bool time_sync_is_valid(void);

/* Unix time in ms. Before a successful sync this returns uptime in ms,
 * which is clearly wrong (1970) and easy to spot in the database. */
uint64_t time_now_ms(void);

/* Converts a k_uptime_get() value to Unix time in ms using the latest sync.
 * Samples are stamped with uptime and converted when sent, so readings
 * taken before the first successful sync still get correct timestamps. */
uint64_t time_uptime_to_epoch_ms(int64_t uptime_ms);
