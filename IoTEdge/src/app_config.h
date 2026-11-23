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

/* How often the sensor is read (kravspec Projekt A: 5-15 s). Can be set at
 * build time, e.g. -DAPP_SAMPLE_INTERVAL_S=600 for one sample per 10 min. */
#ifndef APP_SAMPLE_INTERVAL_S
#define APP_SAMPLE_INTERVAL_S   10
#endif

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
 * Override with -DEXTRA_CFLAGS="-DAPP_UPLINK_CELLULAR=0 -DAPP_ALLOW_INSECURE"
 * (test only: the Wi-Fi uplink has no TLS). */
#ifndef APP_UPLINK_CELLULAR
#define APP_UPLINK_CELLULAR     1
#endif
#define APP_MODEM_APN           "onomondo"
/* How long to wait for LTE registration in a session before giving up. */
#define APP_MODEM_ATTACH_MS     120000

/* MQTT over TLS (issue #22). Cellular uplink only: TLS runs inside the
 * modem. The broker's TLS listener is on APP_MQTT_TLS_PORT; the CA its
 * certificate is checked against lives in mqtt_ca_cert.h.
 * Override with -DEXTRA_CFLAGS="-DAPP_MQTT_TLS=0 -DAPP_ALLOW_INSECURE" for
 * plain MQTT on 1883 (test only). */
#ifndef APP_MQTT_TLS
#define APP_MQTT_TLS            1
#endif
#define APP_MQTT_TLS_PORT       8883

/* IEC 62443 CR 3.1/3.8: never send without a verified server. Plain MQTT
 * (APP_MQTT_TLS = 0) and the Wi-Fi uplink (no TLS at all) are test builds
 * only, so they must be asked for on purpose with -DAPP_ALLOW_INSECURE.
 * Without the CA in mqtt_ca_cert.h the modem refuses to connect
 * (modem.c, tls_configure()). */
#if (!APP_MQTT_TLS || !APP_UPLINK_CELLULAR) && !defined(APP_ALLOW_INSECURE)
#error "Uplink without verified TLS: build with -DAPP_ALLOW_INSECURE (test only)"
#endif

/* ---- Alarms (alarm.c) ----
 * Evaluated by the Edge on every SimPLC reading; the result is written to
 * the SimPLC's coils (buzzer/lamp there). No limits in the kravspec: these
 * were chosen by the group. Values x10, like the Modbus registers. An alarm
 * turns off again only HYST below/above the limit, so a value right at the
 * limit doesn't make it flicker (the DHT11 only reads whole degrees/%).
 * The SimPLC has one temperature coil per position: it means "temperature
 * out of range" (too hot or too cold). */
#ifndef APP_ALARM_TEMP_HIGH_X10
#define APP_ALARM_TEMP_HIGH_X10   400   /* alarm above 40.0 C */
#endif
#ifndef APP_ALARM_TEMP_LOW_X10
#define APP_ALARM_TEMP_LOW_X10    100   /* alarm below 10.0 C */
#endif
#ifndef APP_ALARM_TEMP_HYST_X10
#define APP_ALARM_TEMP_HYST_X10   10    /* 1.0 C */
#endif
#ifndef APP_ALARM_HUM_HIGH_X10
#define APP_ALARM_HUM_HIGH_X10    800   /* alarm at 80.0 % or more */
#endif
#ifndef APP_ALARM_HUM_HYST_X10
#define APP_ALARM_HUM_HYST_X10    30    /* 3.0 % */
#endif

/* ---- Deep sleep design (wake_cycle.c, PO feedback) ----
 * 1 = the SimPLC samples every 10 s into its buffer; the Edge is in deep
 * sleep and wakes every APP_ALARM_CHECK_S to check alarms. It sends (modem)
 * when the oldest unsent sample is APP_SEND_AGE_S old, or at once when an
 * alarm changes. 0 = the light sleep design (Edge samples every 10 s). */
#ifndef APP_DEEP_SLEEP
#define APP_DEEP_SLEEP            0
#endif
#ifndef APP_ALARM_CHECK_S
#define APP_ALARM_CHECK_S         300
#endif
#ifndef APP_SEND_AGE_S
#define APP_SEND_AGE_S            (3600 - APP_ALARM_CHECK_S)
#endif

/* ---- Power saving (see IoTEdge/tests/power/results/RESULTS.md) ---- */

/* 1 = light sleep between work (CONFIG_PM). Measured: ESP idle 49 mA vs
 * light sleep 17.8 mA (modem on) / 7.7 mA (modem radio off), whole system.
 * The USB console stops after the first light sleep: use 0 to debug. */
#ifndef APP_LIGHT_SLEEP
#define APP_LIGHT_SLEEP         (!APP_DEEP_SLEEP) /* deep sleep: awake only briefly */
#endif

/* 1 = turn the modem's radio off (AT+CFUN=0) between sessions and back on
 * at the start of the next one. Saves ~8-10 mA for ~0.4 C per re-attach. */
#ifndef APP_MODEM_RADIO_OFF
#define APP_MODEM_RADIO_OFF     1
#endif

/* 1 = Wi-Fi only on for each sample (connect, read the SimPLC, disconnect),
 * so the ESP can sleep in between. 0 = Wi-Fi stays connected (no light
 * sleep while it is up). Default: on demand when samples are >= 60 s apart;
 * re-connecting every 10 s is unmeasured and phone hotspots dislike it. */
#ifndef APP_WIFI_ON_DEMAND
#define APP_WIFI_ON_DEMAND      (APP_DEEP_SLEEP || APP_SAMPLE_INTERVAL_S >= 60)
#endif
/* Max time for one on-demand Wi-Fi connect + DHCP before the sample is
 * given up (sent as null). */
#define APP_WIFI_CONNECT_TIMEOUT_S 20

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
