/* sparkplug.c - Sparkplug B payloads with nanopb.
 *
 * Sparkplug B rules implemented here:
 *  - seq:   0 in NBIRTH, +1 for every following NDATA, wraps 255 -> 0.
 *           NDEATH carries no seq.
 *  - bdSeq: same value in the NDEATH Will and the NBIRTH of one session,
 *           +1 for every new MQTT session (wraps 255 -> 0).
 *  - NBIRTH must contain every metric (name + alias + datatype) and the
 *           "Node Control/Rebirth" boolean so a host can request a rebirth.
 *  - NDATA  uses aliases only, which keeps the payload small.
 *
 * No Zephyr dependencies except logging, so it can also be compiled and
 * tested on a PC.
 */
#include "sparkplug.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

/* Generated at build time from proto/sparkplug_b.proto (see CMakeLists.txt) */
#include "proto/sparkplug_b.pb.h"

#ifdef __ZEPHYR__
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sparkplug, LOG_LEVEL_INF);
#else
#define LOG_ERR(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

typedef org_eclipse_tahu_protobuf_Payload        sp_payload_t;
typedef org_eclipse_tahu_protobuf_Payload_Metric sp_metric_t;

#define SP_DT_FLOAT   org_eclipse_tahu_protobuf_DataType_Float
#define SP_DT_UINT64  org_eclipse_tahu_protobuf_DataType_UInt64
#define SP_DT_BOOLEAN org_eclipse_tahu_protobuf_DataType_Boolean

#define TAG_FLOAT   org_eclipse_tahu_protobuf_Payload_Metric_float_value_tag
#define TAG_LONG    org_eclipse_tahu_protobuf_Payload_Metric_long_value_tag
#define TAG_BOOLEAN org_eclipse_tahu_protobuf_Payload_Metric_boolean_value_tag

#define MAX_METRICS (sizeof(((sp_payload_t *)0)->metrics) / sizeof(sp_metric_t))

/* The payload struct is ~2.6 kB, so it is static instead of on the stack. */
static sp_payload_t payload;
static uint8_t seq;
static uint8_t bd_seq;
static bool first_session = true;

static const char *const type_names[] = {
	[SP_MSG_NBIRTH] = "NBIRTH",
	[SP_MSG_NDEATH] = "NDEATH",
	[SP_MSG_NDATA]  = "NDATA",
	[SP_MSG_NCMD]   = "NCMD",
};

int sparkplug_build_topic(enum sparkplug_msg_type type, const char *node_id,
			  char *buf, size_t len)
{
	int n = snprintf(buf, len, SPARKPLUG_NAMESPACE "/%s/%s/%s",
			 SPARKPLUG_GROUP_ID, type_names[type], node_id);

	return (n < 0 || (size_t)n >= len) ? -ENOMEM : 0;
}

void sparkplug_begin_session(void)
{
	if (first_session) {
		first_session = false;
		bd_seq = 0;
	} else {
		bd_seq++; /* uint8_t wraps 255 -> 0 by itself */
	}
}

static void payload_reset(uint64_t now_ms, bool with_seq)
{
	memset(&payload, 0, sizeof(payload));
	payload.has_timestamp = true;
	payload.timestamp = now_ms;
	if (with_seq) {
		payload.has_seq = true;
		payload.seq = seq++;
	}
}

/* Adds an empty metric. A name is only set in BIRTH/DEATH; alias 0 = none. */
static sp_metric_t *metric_add(const char *name, uint64_t alias,
			       uint32_t datatype, uint64_t ts_ms)
{
	if (payload.metrics_count >= MAX_METRICS) {
		return NULL;
	}

	sp_metric_t *m = &payload.metrics[payload.metrics_count++];

	if (name) {
		m->has_name = true;
		strncpy(m->name, name, sizeof(m->name) - 1);
	}
	if (alias) {
		m->has_alias = true;
		m->alias = alias;
	}
	m->has_datatype = true;
	m->datatype = datatype;
	m->has_timestamp = true;
	m->timestamp = ts_ms;
	return m;
}

static int payload_encode(uint8_t *buf, size_t len)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, len);

	if (!pb_encode(&stream, org_eclipse_tahu_protobuf_Payload_fields, &payload)) {
		LOG_ERR("pb_encode failed: %s", PB_GET_ERROR(&stream));
		return -ENOMEM;
	}
	return (int)stream.bytes_written;
}

static int add_bdseq(uint64_t now_ms)
{
	sp_metric_t *m = metric_add(SP_METRIC_BDSEQ, 0, SP_DT_UINT64, now_ms);

	if (!m) {
		return -ENOMEM;
	}
	m->which_value = TAG_LONG;
	m->value.long_value = bd_seq;
	return 0;
}

int sparkplug_encode_ndeath(uint8_t *buf, size_t len)
{
	/* The Will is created now but published by the broker later, so the
	 * timestamp is only "when the session started". The spec allows this. */
	payload_reset(0, false);
	payload.has_timestamp = false;

	if (add_bdseq(0)) {
		return -ENOMEM;
	}
	payload.metrics[0].has_timestamp = false;
	return payload_encode(buf, len);
}

int sparkplug_encode_nbirth(uint8_t *buf, size_t len, uint64_t now_ms,
			    const struct sparkplug_sample *latest)
{
	sp_metric_t *m;

	seq = 0;
	payload_reset(now_ms, true);

	if (add_bdseq(now_ms)) {
		return -ENOMEM;
	}

	m = metric_add(SP_METRIC_REBIRTH, SP_ALIAS_REBIRTH, SP_DT_BOOLEAN, now_ms);
	if (!m) {
		return -ENOMEM;
	}
	m->which_value = TAG_BOOLEAN;
	m->value.boolean_value = false;

	m = metric_add(SP_METRIC_TEMPERATURE, SP_ALIAS_TEMPERATURE, SP_DT_FLOAT,
		       latest ? latest->timestamp_ms : now_ms);
	if (!m) {
		return -ENOMEM;
	}
	if (latest) {
		m->which_value = TAG_FLOAT;
		m->value.float_value = latest->temperature_c;
	} else {
		/* No reading yet: the metric still has to be declared in BIRTH. */
		m->has_is_null = true;
		m->is_null = true;
	}

	return payload_encode(buf, len);
}

int sparkplug_encode_ndata(uint8_t *buf, size_t len, uint64_t now_ms,
			   const struct sparkplug_sample *samples, size_t count)
{
	if (count == 0 || count > MAX_METRICS) {
		return -EINVAL;
	}

	payload_reset(now_ms, true);

	for (size_t i = 0; i < count; i++) {
		sp_metric_t *m = metric_add(NULL, SP_ALIAS_TEMPERATURE, SP_DT_FLOAT,
					    samples[i].timestamp_ms);

		m->which_value = TAG_FLOAT;
		m->value.float_value = samples[i].temperature_c;
		/* datatype was declared in NBIRTH; leaving it out saves 2 B/sample */
		m->has_datatype = false;

		if (i + 1 < count) {
			m->has_is_historical = true;
			m->is_historical = true;
		}
	}

	return payload_encode(buf, len);
}

bool sparkplug_ncmd_is_rebirth(const uint8_t *buf, size_t len)
{
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	memset(&payload, 0, sizeof(payload));
	if (!pb_decode(&stream, org_eclipse_tahu_protobuf_Payload_fields, &payload)) {
		LOG_ERR("NCMD decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	for (pb_size_t i = 0; i < payload.metrics_count; i++) {
		const sp_metric_t *m = &payload.metrics[i];
		bool is_rebirth =
			(m->has_name && strcmp(m->name, SP_METRIC_REBIRTH) == 0) ||
			(m->has_alias && m->alias == SP_ALIAS_REBIRTH);

		if (is_rebirth && m->which_value == TAG_BOOLEAN &&
		    m->value.boolean_value) {
			return true;
		}
	}
	return false;
}
