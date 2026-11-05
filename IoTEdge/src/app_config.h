/* app_config.h - tunables for sampling, batching and Sparkplug naming.
 *
 * Everything that changes the data/power trade-off lives here, so it can be
 * tuned in one place (and argued for in the report).
 */
#pragma once

/* Sparkplug B group id: first level after "spBv1.0/" in every topic.
 * Keep it short - it is sent in every single topic string. */
#ifndef SPARKPLUG_GROUP_ID
#define SPARKPLUG_GROUP_ID "UCL"
#endif

/* How often the sensor is read (kravspec Projekt A: 5-15 s). */
#define APP_SAMPLE_INTERVAL_S   10

/* How often the node connects and sends everything it has collected
 * (option A: connect -> NBIRTH -> NDATA... -> NDEATH -> disconnect).
 * 3600 = hourly. Use something like 60 while developing. */
#ifndef APP_PUBLISH_INTERVAL_S
#define APP_PUBLISH_INTERVAL_S  3600
#endif

/* After a failed session (no network, broker down) try again this soon. */
#define APP_RETRY_INTERVAL_S    300

/* Max samples per NDATA message. One hour at 10 s = 360 samples, which is
 * sent as 6 messages of ~1 kB. Must match max_count in
 * proto/sparkplug_b.options. */
#define APP_NDATA_MAX_SAMPLES   60

/* Samples kept in RAM (16 B each). Two publish intervals, so one failed
 * session loses nothing. When full, the oldest sample is dropped. */
#define APP_SAMPLE_QUEUE_LEN    (2 * APP_PUBLISH_INTERVAL_S / APP_SAMPLE_INTERVAL_S)

/* SNTP: real UTC time for Sparkplug timestamps. Re-synced before a session
 * when the last sync is older than this, to correct clock drift. */
#define APP_SNTP_SERVER         "pool.ntp.org"
#define APP_SNTP_RESYNC_S       (24 * 3600)
