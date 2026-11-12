/* app_config.h - tunables for sampling, batching and Sparkplug naming.
 *
 * Everything that changes the data/power trade-off lives here, so it can be
 * tuned in one place (and argued for in the report).
 */
#pragma once

/* Sparkplug B group id: first level after "spBv1.0/" in every topic.
 * Keep it short - it is sent in every single topic string. */
#ifndef SPARKPLUG_GROUP_ID
#define SPARKPLUG_GROUP_ID "GRP1"
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
#ifndef APP_RETRY_INTERVAL_S
#define APP_RETRY_INTERVAL_S    300
#endif

/* Samples per NDATA message follow from the metric limit and the uplink's
 * message size (see SPARKPLUG_MAX_SAMPLES_PER_NDATA and mqtt_client.c).
 * One hour at 10 s = 360 samples of 7 metrics each: over cellular that is
 * ~52 messages of ~0.9 kB per session. */

/* Samples kept in RAM (40 B each, ~29 kB for 720). Two publish intervals, so
 * one failed session loses nothing. When full, the oldest sample is dropped. */
#define APP_SAMPLE_QUEUE_LEN    (2 * APP_PUBLISH_INTERVAL_S / APP_SAMPLE_INTERVAL_S)

/* Uplink to the broker: 1 = SIM7080G cellular modem (LTE-M) on UART1,
 * 0 = Wi-Fi with Zephyr's own MQTT client. Wi-Fi is started either way,
 * because Modbus TCP to the SimPLC runs over the local Wi-Fi.
 * Override with -DEXTRA_CFLAGS=-DAPP_UPLINK_CELLULAR=0 */
#ifndef APP_UPLINK_CELLULAR
#define APP_UPLINK_CELLULAR     1
#endif
#define APP_MODEM_APN           "onomondo"
/* How long to wait for LTE registration in a session before giving up. */
#define APP_MODEM_ATTACH_MS     120000

/* MQTT over TLS (issue #22). Cellular uplink only: TLS runs inside the
 * modem. The broker's TLS listener is on APP_MQTT_TLS_PORT; the CA its
 * certificate is checked against lives in mqtt_ca_cert.h.
 * Override with -DEXTRA_CFLAGS=-DAPP_MQTT_TLS=0 for plain MQTT on 1883. */
#ifndef APP_MQTT_TLS
#define APP_MQTT_TLS            1
#endif
#define APP_MQTT_TLS_PORT       8883

/* ---- Power measurement switches (PPK2 tests, see IoTEdge/tests/power) ---- */

/* 1 = drive phase markers on GPIO18-21 for the PPK2 logic port. */
#ifndef APP_POWER_MARKERS
#define APP_POWER_MARKERS       0
#endif

/* 1 = "no power saving": connect once and stay connected, publish every
 * sample right away as its own NDATA (no batching, no disconnect).
 * Cellular uplink only. 0 = option A sessions (the normal design). */
#ifndef APP_PERSISTENT_SESSION
#define APP_PERSISTENT_SESSION  0
#endif

/* SNTP: real UTC time for Sparkplug timestamps. Re-synced before a session
 * when the last sync is older than this, to correct clock drift. */
#define APP_SNTP_SERVER         "pool.ntp.org"
#define APP_SNTP_RESYNC_S       (24 * 3600)
