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
#define SPARKPLUG_PAYLOAD_LEN 1024

/* Metric names as sent in NBIRTH. */
#define SP_METRIC_BDSEQ       "bdSeq"
#define SP_METRIC_REBIRTH     "Node Control/Rebirth"
#define SP_METRIC_TEMPERATURE "Temperature"

/* Aliases: sent together with the name in NBIRTH, and alone in NDATA.
 * bdSeq has no alias, because it is only ever sent in NBIRTH/NDEATH. */
enum sparkplug_alias {
	SP_ALIAS_REBIRTH     = 1,
	SP_ALIAS_TEMPERATURE = 2,
};

enum sparkplug_msg_type {
	SP_MSG_NBIRTH,
	SP_MSG_NDEATH,
	SP_MSG_NDATA,
	SP_MSG_NCMD,
};

/* One sensor reading, stamped when it was taken (ms since Unix epoch, UTC). */
struct sparkplug_sample {
	uint64_t timestamp_ms;
	float temperature_c;
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

/* NDATA: `count` samples by alias only. All but the newest sample are
 * flagged is_historical, so a host knows they are buffered, older values. */
int sparkplug_encode_ndata(uint8_t *buf, size_t len, uint64_t now_ms,
			   const struct sparkplug_sample *samples, size_t count);

/* True if an NCMD payload asks for a rebirth (by name or alias). */
bool sparkplug_ncmd_is_rebirth(const uint8_t *buf, size_t len);
