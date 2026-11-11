/* time_sync.h - wall-clock time for Sparkplug timestamps.
 *
 * Sparkplug timestamps are milliseconds since the Unix epoch (UTC). The
 * ESP32 has no battery-backed clock, so we fetch UTC once and keep an offset
 * to the kernel uptime counter.
 *
 * Where UTC comes from:
 *  - Wi-Fi: SNTP over Zephyr's network stack (default, no source set).
 *  - Cellular: the SIM7080G has its own IP stack, so Zephyr's SNTP can't reach
 *    the network. Set a source that asks the modem instead, e.g.
 *        static int modem_time(int64_t *ms) { return modem_get_utc_ms(APP_SNTP_SERVER, ms); }
 *        time_sync_set_source(modem_time);
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Returns 0 and writes UTC in ms since the Unix epoch, or a negative errno. */
typedef int (*time_sync_source_t)(int64_t *epoch_ms);

/* NULL (default) = SNTP. Call before time_sync_initialize(). The source is
 * called from the thread that calls time_sync_initialize()/refresh_if_stale(). */
void time_sync_set_source(time_sync_source_t src);

/* Blocking: fetch UTC from the source (or SNTP, a few retries). Needs the
 * network to be up. */
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
