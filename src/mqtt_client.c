/* mqtt_client.c - MQTT transport for the Sparkplug B edge node.
 *
 * "Option A": the node is only connected while it has something to send.
 * Every APP_PUBLISH_INTERVAL_S the MQTT thread runs one short session:
 *
 *   1. sparkplug_begin_session()        -> new bdSeq
 *   2. CONNECT, NDEATH registered as Will (QoS 1, not retained, clean session)
 *   3. CONNACK -> NBIRTH (seq 0)
 *   4. all queued samples as NDATA, max APP_NDATA_MAX_SAMPLES per message
 *   5. NDEATH (same bdSeq) published by us, then DISCONNECT
 *
 * Step 5 matters: a clean DISCONNECT tells the broker to throw the Will away,
 * so without our own NDEATH the host would think the node is still online.
 * If the node dies mid-session (power loss, network gone), the broker
 * publishes the Will instead, so the host learns about it either way.
 *
 * The node does not subscribe to NCMD: it is offline most of the time, and
 * every session starts with a full NBIRTH anyway, which is what a rebirth
 * request would ask for.
 *
 * All MQTT and sparkplug_* calls happen in this one thread. Other threads only
 * touch the sample queue.
 */
#include "mqtt_client.h"
#include "mqtt_settings.h"
#include "client_id.h"
#include "time_sync.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_INF);

#define RX_BUF_SIZE        256
#define TX_BUF_SIZE        256
#define STACK_SIZE         4096
/* Sessions last seconds, but the keepalive also decides how fast the broker
 * notices a dead node (1.5 x keepalive) and publishes the NDEATH Will. */
#define KEEPALIVE_S        60U
#define CONNACK_TIMEOUT_MS 10000

BUILD_ASSERT(APP_SAMPLE_QUEUE_LEN >= APP_PUBLISH_INTERVAL_S / APP_SAMPLE_INTERVAL_S,
	     "sample queue must hold at least one publish interval of samples");

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];

static K_THREAD_STACK_DEFINE(mqtt_stack, STACK_SIZE);
static struct k_thread mqtt_thread_data;

K_MSGQ_DEFINE(sample_q, sizeof(struct sparkplug_sample), APP_SAMPLE_QUEUE_LEN, 8);
/* Counts samples dropped because the queue was full. The oldest samples are
 * the ones being published, so a drop during a publish means fewer items
 * are left to remove afterwards. */
static atomic_t samples_dropped;

/* Sparkplug topics, built once we know our node id */
static char topic_nbirth[SPARKPLUG_TOPIC_LEN];
static char topic_ndeath[SPARKPLUG_TOPIC_LEN];
static char topic_ndata[SPARKPLUG_TOPIC_LEN];

/* The Will buffer must stay valid for the whole session. */
static uint8_t ndeath_buf[32];
static int ndeath_len;
static uint8_t payload_buf[SPARKPLUG_PAYLOAD_LEN];
static struct sparkplug_sample batch[APP_NDATA_MAX_SAMPLES];

static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg;

/* Set from the event handler (called from mqtt_input in this thread) */
static bool connected;
static int connack_result;

/* Last sample sent, so NBIRTH can report a current value */
static struct sparkplug_sample last_sample;
static bool have_last_sample;

static struct mqtt_utf8 user = {
    .utf8 = (uint8_t *)MQTT_ESP32_USER,
    .size = sizeof(MQTT_ESP32_USER) - 1,
};
static struct mqtt_utf8 pass = {
    .utf8 = (uint8_t *)MQTT_ESP32_PASS,
    .size = sizeof(MQTT_ESP32_PASS) - 1,
};

static void mqtt_thread_fn(void *p1, void *p2, void *p3);

int mqtt_client_initialize(void)
{
	k_thread_create(&mqtt_thread_data, mqtt_stack,
			K_THREAD_STACK_SIZEOF(mqtt_stack),
			mqtt_thread_fn,
			NULL, NULL, NULL,
			5, 0, K_NO_WAIT);

	k_thread_name_set(&mqtt_thread_data, "mqtt_client");
	return 0;
}

int mqtt_client_queue_sample(const struct sparkplug_sample *sample)
{
	struct sparkplug_sample dropped;

	while (k_msgq_put(&sample_q, sample, K_NO_WAIT) != 0) {
		/* full: drop the oldest and try again */
		if (k_msgq_get(&sample_q, &dropped, K_NO_WAIT) == 0) {
			atomic_inc(&samples_dropped);
			LOG_WRN("Sample queue full, dropped oldest sample");
		}
	}
	return 0;
}

/* Sparkplug: NBIRTH/NDATA/NDEATH are QoS 0 and never retained */
static int publish(const char *topic, const uint8_t *data, size_t len)
{
	struct mqtt_publish_param param = {0};

	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.topic.topic.utf8 = (const uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.payload.data = (uint8_t *)data;
	param.message.payload.len = len;

	return mqtt_publish(&client, &param);
}

static int publish_nbirth(void)
{
	int len = sparkplug_encode_nbirth(payload_buf, sizeof(payload_buf),
					  time_now_ms(),
					  have_last_sample ? &last_sample : NULL);
	if (len < 0) {
		return len;
	}

	int ret = publish(topic_nbirth, payload_buf, len);

	if (ret == 0) {
		LOG_INF("NBIRTH sent (%d bytes)", len);
	}
	return ret;
}

/* Sends one NDATA with up to APP_NDATA_MAX_SAMPLES queued samples.
 * Returns the number of samples sent (0 = queue empty) or a negative errno.
 * Samples are only removed from the queue once mqtt_publish() succeeded. */
static int publish_ndata_chunk(void)
{
	size_t n = 0;
	atomic_val_t dropped_before = atomic_get(&samples_dropped);

	while (n < APP_NDATA_MAX_SAMPLES &&
	       k_msgq_peek_at(&sample_q, &batch[n], n) == 0) {
		n++;
	}
	if (n == 0) {
		return 0;
	}

	/* Queued samples carry uptime; convert to UTC with the latest sync */
	for (size_t i = 0; i < n; i++) {
		batch[i].timestamp_ms = time_uptime_to_epoch_ms((int64_t)batch[i].timestamp_ms);
	}

	int len = sparkplug_encode_ndata(payload_buf, sizeof(payload_buf),
					 time_now_ms(), batch, n);
	if (len < 0) {
		return len;
	}

	int ret = publish(topic_ndata, payload_buf, len);

	if (ret) {
		return ret;
	}

	size_t dropped_meanwhile = atomic_get(&samples_dropped) - dropped_before;
	struct sparkplug_sample discard;

	for (size_t i = dropped_meanwhile; i < n; i++) {
		k_msgq_get(&sample_q, &discard, K_NO_WAIT);
	}
	last_sample = batch[n - 1];
	have_last_sample = true;
	LOG_INF("NDATA sent: %u sample(s), %d bytes", (unsigned int)n, len);
	return (int)n;
}

static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connack_result = evt->result;
		if (evt->result != 0) {
			LOG_ERR("CONNACK refused, code %d", evt->result);
			break;
		}
		LOG_INF("MQTT connected");
		connected = true;
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected: %d", evt->result);
		connected = false;
		break;

	default:
		break;
	}
}

#ifdef MQTT_DEBUG_RAW_TCP_TEST
/* Plain TCP connect test. Off by default: it opens an extra connection on
 * every session, which costs data on cellular. Define it in
 * mqtt_settings.h to turn it back on while debugging. */
static int test_raw_tcp_socket(void)
{
    int ret;
    int sock;
    struct sockaddr_in addr = {0};

    sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        printk("socket failed: %d\n", sock);
        return sock;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(MQTT_BROKER_PORT);

    ret = zsock_inet_pton(AF_INET, MQTT_BROKER_HOSTNAME, &addr.sin_addr);
    if (ret <= 0) {
        printk("inet_pton failed: %d\n", ret);
        zsock_close(sock);
        return -EINVAL;
    }

    ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    printk("raw connect ret = %d\n", ret);

    zsock_close(sock);
    return ret;
}
#endif

static int connect_mqtt(void)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	const char *id = build_client_id_get();

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(MQTT_BROKER_PORT);

	if (zsock_inet_pton(AF_INET, MQTT_BROKER_HOSTNAME, &broker4->sin_addr) != 1) {
		LOG_ERR("Invalid broker IP");
		return -EINVAL;
	}

#ifdef MQTT_DEBUG_RAW_TCP_TEST
	int ret = test_raw_tcp_socket();

	if (ret < 0) {
		return ret;
	}
#endif

	/* New session: new bdSeq. The same NDEATH bytes are used as the Will
	 * and as our own NDEATH at the end of the session. */
	sparkplug_begin_session();
	ndeath_len = sparkplug_encode_ndeath(ndeath_buf, sizeof(ndeath_buf));
	if (ndeath_len < 0) {
		return ndeath_len;
	}
	will_topic.topic.utf8 = (const uint8_t *)topic_ndeath;
	will_topic.topic.size = strlen(topic_ndeath);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE; /* Sparkplug requires QoS 1 */
	will_msg.utf8 = ndeath_buf;
	will_msg.size = ndeath_len;

	mqtt_client_init(&client);
	client.user_name = &user;
	client.password  = &pass;
	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (uint8_t *)id;
	client.client_id.size = strlen(id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.keepalive = KEEPALIVE_S;
	client.clean_session = 1;          /* Sparkplug requires clean session */
	client.will_topic = &will_topic;
	client.will_message = &will_msg;
	client.will_retain = 0;            /* Sparkplug: NDEATH is never retained */
	client.transport.type = MQTT_TRANSPORT_NON_SECURE; /* TODO: TLS */

	connected = false;
	connack_result = 0;
	return mqtt_connect(&client);
}

/* Processes incoming packets until CONNACK arrives (or timeout/refusal). */
static int wait_for_connack(void)
{
	struct zsock_pollfd fds = {
		.fd = client.transport.tcp.sock,
		.events = ZSOCK_POLLIN,
	};
	int64_t deadline = k_uptime_get() + CONNACK_TIMEOUT_MS;

	while (!connected) {
		int remaining = (int)(deadline - k_uptime_get());

		if (remaining <= 0) {
			LOG_ERR("No CONNACK from broker");
			return -ETIMEDOUT;
		}

		int rc = zsock_poll(&fds, 1, remaining);

		if (rc < 0) {
			return -errno;
		}
		if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
			return -ECONNRESET;
		}
		if (fds.revents & ZSOCK_POLLIN) {
			rc = mqtt_input(&client);
			if (rc < 0) {
				return rc;
			}
			if (connack_result != 0) {
				return -ECONNREFUSED;
			}
		}
	}
	return 0;
}

/* One complete option A session. On any error the connection is aborted;
 * the broker then publishes the Will, and unsent samples stay queued. */
static int run_session(void)
{
	int rc = connect_mqtt();

	if (rc) {
		LOG_ERR("mqtt_connect failed: %d", rc);
		return rc;
	}

	rc = wait_for_connack();
	if (rc == 0) {
		rc = publish_nbirth();
	}

	while (rc == 0) {
		int sent = publish_ndata_chunk();

		if (sent <= 0) {
			rc = sent;
			break;
		}
	}

	if (rc == 0) {
		/* Leave cleanly: NDEATH ourselves, then DISCONNECT (drops the Will) */
		rc = publish(topic_ndeath, ndeath_buf, ndeath_len);
		if (rc == 0) {
			rc = mqtt_disconnect(&client, NULL);
		}
		if (rc == 0) {
			LOG_INF("Session done, disconnected");
			return 0;
		}
	}

	LOG_ERR("Session failed: %d", rc);
	mqtt_abort(&client);
	return rc;
}

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const char *node_id = build_client_id_get();

	sparkplug_build_topic(SP_MSG_NBIRTH, node_id, topic_nbirth, sizeof(topic_nbirth));
	sparkplug_build_topic(SP_MSG_NDEATH, node_id, topic_ndeath, sizeof(topic_ndeath));
	sparkplug_build_topic(SP_MSG_NDATA,  node_id, topic_ndata,  sizeof(topic_ndata));
	LOG_INF("Sparkplug node topics: %s", topic_ndata);

	/* First session right away, so a fresh boot shows up on the host */
	int64_t next = k_uptime_get();

	while (1) {
		k_sleep(K_TIMEOUT_ABS_MS(next));

		time_sync_refresh_if_stale();
		if (!time_sync_is_valid()) {
			LOG_WRN("No valid time: timestamps will be uptime-based");
		}

		if (run_session() == 0) {
			next += (int64_t)APP_PUBLISH_INTERVAL_S * MSEC_PER_SEC;
		} else {
			/* Samples are still queued; try again sooner than a full
			 * interval, but don't burn power on a dead network. */
			next = k_uptime_get() + (int64_t)APP_RETRY_INTERVAL_S * MSEC_PER_SEC;
			LOG_INF("Retrying in %d s", APP_RETRY_INTERVAL_S);
		}
	}
}
