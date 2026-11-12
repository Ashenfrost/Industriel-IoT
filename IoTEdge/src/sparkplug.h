/* sparkplug.h - Sparkplug B (spBv1.0) topics and payload encoding.
 *
 * This module knows nothing about sockets or MQTT; it only builds topic
 * strings and turns data into protobuf bytes (via nanopb), and back.
 *
 * Not thread safe: call it from the MQTT thread only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#define SPARKPLUG_NAMESPACE   "spBv1.0"
#define SPARKPLUG_TOPIC_LEN   96
#define SPARKPLUG_PAYLOAD_LEN 1280 /* fits SPARKPLUG_MAX_SAMPLES_PER_NDATA */

/* Data comes from the SimPLC over Modbus TCP: 3 DHT11 sensors (0 = top,
 * 1 = middle, 2 = bottom), each with temperature and humidity, plus the
 * PLC's status bitmask (bit i = sensor i failed its last read). */
#define SP_NUM_SENSORS        3

/* Metric names as sent in NBIRTH (max 23 chars, see sparkplug_b.options). */
#define SP_METRIC_BDSEQ       "bdSeq"
#define SP_METRIC_REBIRTH     "Node Control/Rebirth"
#define SP_METRIC_PLC_STATUS  "PLC/Status"
/* Per sensor: "<position>/Temperature" and "<position>/Humidity" */
#define SP_POSITION_NAMES     { "Top", "Middle", "Bottom" }

/* Aliases: sent together with the name in NBIRTH, and alone in NDATA.
 * bdSeq has no alias, because it is only ever sent in NBIRTH/NDEATH.
 * Alias 2 was the ESP32's own die temperature (no longer sent); it is not
 * reused, so old data in the host can't be mixed up with new metrics. */
enum sparkplug_alias {
	SP_ALIAS_REBIRTH     = 1,
	SP_ALIAS_TEMP_BASE   = 10,   /* 10, 11, 12 = top, middle, bottom temperature */
	SP_ALIAS_HUM_BASE    = 20,   /* 20, 21, 22 = top, middle, bottom humidity */
	SP_ALIAS_PLC_STATUS  = 30,
};

/* Metrics in one sample: temperature + humidity per sensor, and the status */
#define SP_METRICS_PER_SAMPLE (2 * SP_NUM_SENSORS + 1)

/* Max metrics in one payload. Must match max_count of Payload.metrics in
 * proto/sparkplug_b.options (checked at build time in sparkplug.c). */
#define SPARKPLUG_MAX_METRICS 60

/* Most samples that fit in one NDATA by metric count (8). The uplink may
 * allow fewer because of its message size limit (see mqtt_client.c). */
#define SPARKPLUG_MAX_SAMPLES_PER_NDATA (SPARKPLUG_MAX_METRICS / SP_METRICS_PER_SAMPLE)

enum sparkplug_msg_type {
	SP_MSG_NBIRTH,
	SP_MSG_NDEATH,
	SP_MSG_NDATA,
	SP_MSG_NCMD,
};

/* One reading of all PLC values. timestamp_ms is ms since Unix epoch (UTC)
 * when it is encoded; while queued on the device it holds k_uptime_get()
 * instead.
 *
 * plc_ok = false: no fresh Modbus data for this sample, so every value is
 * sent as null. Otherwise sensor i is null when bit i of plc_status is set
 * (the PLC kept its old value after a failed DHT11 read). */
struct sparkplug_sample {
	uint64_t timestamp_ms;
	float temperature_c[SP_NUM_SENSORS];
	float humidity_pct[SP_NUM_SENSORS];
	uint16_t plc_status;
	bool plc_ok;
};

/* Builds "spBv1.0/<group>/<type>/<node_id>". Returns 0 or -ENOMEM. */
int sparkplug_build_topic(enum sparkplug_msg_type type, const char *node_id,
			  char *buf, size_t len);

/* Starts a new MQTT session: bumps bdSeq (0 on the very first session).
 * Call once before every mqtt_connect(), then encode the NDEATH Will. */
void sparkplug_begin_session(void);

/* The encoders return the number of bytes written, or a negative errno. */

/* NDEATH: registered as MQTT Will at connect. Contains only bdSeq. */
int sparkplug_encode_ndeath(uint8_t *buf, size_t len);

/* NBIRTH: every metric with name, alias, datatype and current value.
 * Resets seq to 0. `latest` may be NULL if nothing has been sampled yet. */
int sparkplug_encode_nbirth(uint8_t *buf, size_t len, uint64_t now_ms,
			    const struct sparkplug_sample *latest);

/* NDATA: `count` samples (max SPARKPLUG_MAX_SAMPLES_PER_NDATA), every value
 * by alias only. All but the newest sample are flagged is_historical, so a
 * host knows they are buffered, older values. */
int sparkplug_encode_ndata(uint8_t *buf, size_t len, uint64_t now_ms,
			   const struct sparkplug_sample *samples, size_t count);

/* True if an NCMD payload asks for a rebirth (by name or alias).
 * Not used with option A (the node doesn't subscribe to NCMD), kept for
 * when the node stays connected or commands are added. */
bool sparkplug_ncmd_is_rebirth(const uint8_t *buf, size_t len);
