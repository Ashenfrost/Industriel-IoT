/* mqtt_client.c - MQTT transport for the Sparkplug B edge node.
 *
 * Session life cycle (one loop iteration of mqtt_thread_fn):
 *   1. sparkplug_begin_session()   -> new bdSeq
 *   2. CONNECT with NDEATH as Will  (QoS 1, not retained, clean session)
 *   3. CONNACK -> subscribe NCMD, then publish NBIRTH (seq 0)
 *   4. publish queued samples as NDATA (seq 1, 2, ...)
 *   5. on any error: abort, wait (backoff), go to 1
 *
 * All MQTT calls and all sparkplug_* calls happen in this one thread, so no
 * locking is needed. Other threads only touch the sample queue.
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

#define RX_BUF_SIZE  512
#define TX_BUF_SIZE  512
#define STACK_SIZE   4096
#define KEEPALIVE_S  60U
#define BACKOFF_MIN_S 5
#define BACKOFF_MAX_S 60
#define CONNACK_TIMEOUT_MS 10000

BUILD_ASSERT(APP_SAMPLE_QUEUE_LEN >= APP_PUBLISH_BATCH_SIZE,
	     "sample queue must hold at least one batch");

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];
static struct zsock_pollfd fds;

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
static char topic_ncmd[SPARKPLUG_TOPIC_LEN];

/* Payload buffers. The Will must stay valid for the whole session. */
static uint8_t will_buf[64];
static uint8_t payload_buf[SPARKPLUG_PAYLOAD_LEN];

static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg;

/* Set from the event handler, acted on in the thread loop */
static atomic_t connected;
static bool birth_pending;

/* Last sample sent, so a rebirth can report the current value */
static struct sparkplug_sample last_sample;
static bool have_last_sample;

static int connect_mqtt(void);
static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt);
static void mqtt_thread_fn(void *p1, void *p2, void *p3);

static struct mqtt_utf8 user = {
    .utf8 = (uint8_t *)MQTT_ESP32_USER,
    .size = sizeof(MQTT_ESP32_USER) - 1,
};
static struct mqtt_utf8 pass = {
    .utf8 = (uint8_t *)MQTT_ESP32_PASS,
    .size = sizeof(MQTT_ESP32_PASS) - 1,
};

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

bool mqtt_client_is_connected(void)
{
	return atomic_get(&connected) != 0;
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

/* Sparkplug: everything except the NDEATH Will is QoS 0, not retained */
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

/* Sends one NDATA with up to APP_PUBLISH_BATCH_SIZE queued samples.
 * Samples are only removed from the queue once mqtt_publish() succeeded. */
static int publish_ndata_batch(void)
{
	struct sparkplug_sample batch[APP_PUBLISH_BATCH_SIZE];
	size_t n = 0;
	atomic_val_t dropped_before = atomic_get(&samples_dropped);

	while (n < APP_PUBLISH_BATCH_SIZE &&
	       k_msgq_peek_at(&sample_q, &batch[n], n) == 0) {
		n++;
	}
	if (n == 0) {
		return 0;
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
	return 0;
}

static int subscribe_ncmd(void)
{
	struct mqtt_topic t = {
		.topic = {
			.utf8 = (const uint8_t *)topic_ncmd,
			.size = strlen(topic_ncmd),
		},
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	};
	const struct mqtt_subscription_list list = {
		.list = &t,
		.list_count = 1,
		.message_id = 1,
	};

	return mqtt_subscribe(&client, &list);
}

/* Reads an incoming PUBLISH (only NCMD is subscribed) and checks for rebirth. */
static void handle_incoming_publish(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	size_t len = evt->param.publish.message.payload.len;

	if (len > sizeof(payload_buf)) {
		/* Too big for us: read it away in chunks so the stream stays in sync */
		LOG_WRN("NCMD too large (%u bytes), ignored", (unsigned int)len);
		while (len > 0) {
			size_t chunk = MIN(len, sizeof(payload_buf));

			if (mqtt_readall_publish_payload(c, payload_buf, chunk) < 0) {
				return;
			}
			len -= chunk;
		}
		return;
	}

	if (mqtt_readall_publish_payload(c, payload_buf, len) < 0) {
		LOG_ERR("Failed to read NCMD payload");
		return;
	}

	if (sparkplug_ncmd_is_rebirth(payload_buf, len)) {
		LOG_INF("Rebirth requested by host");
		birth_pending = true;
	}
}

#ifdef MQTT_DEBUG_RAW_TCP_TEST
/* Plain TCP connect test. Off by default: it opens an extra connection on
 * every reconnect, which costs data on cellular. Define it in
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
	int ret;
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	const char *id = build_client_id_get();

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(MQTT_BROKER_PORT);

	if (zsock_inet_pton(AF_INET, MQTT_BROKER_HOSTNAME, &broker4->sin_addr) != 1) {
		LOG_ERR("Invalid broker IP");
		return -EINVAL;
	}

#ifdef MQTT_DEBUG_RAW_TCP_TEST
	ret = test_raw_tcp_socket();
	if (ret < 0) {
		return ret;
	}
#endif

	/* New session: new bdSeq, and a Will (NDEATH) carrying that bdSeq */
	sparkplug_begin_session();
	ret = sparkplug_encode_ndeath(will_buf, sizeof(will_buf));
	if (ret < 0) {
		return ret;
	}
	will_topic.topic.utf8 = (const uint8_t *)topic_ndeath;
	will_topic.topic.size = strlen(topic_ndeath);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE; /* Sparkplug requires QoS 1 */
	will_msg.utf8 = will_buf;
	will_msg.size = ret;

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

	return mqtt_connect(&client);
}

static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("CONNACK refused, code %d", evt->result);
			break;
		}
		LOG_INF("MQTT connected");
		atomic_set(&connected, 1);
		/* Don't publish from inside the callback: let the loop do it */
		birth_pending = true;
		break;

	case MQTT_EVT_SUBACK:
		LOG_INF("Subscribed to %s", topic_ncmd);
		break;

	case MQTT_EVT_PUBLISH:
		handle_incoming_publish(c, evt);
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected: %d", evt->result);
		atomic_set(&connected, 0);
		break;

	default:
		break;
	}
}

/* Runs one connected session until something fails. Returns the error. */
static int run_session(void)
{
	int rc;

	int64_t connect_started = k_uptime_get();

	fds.fd = client.transport.tcp.sock;
	fds.events = ZSOCK_POLLIN;

	while (1) {
		if (!mqtt_client_is_connected() &&
		    k_uptime_get() - connect_started > CONNACK_TIMEOUT_MS) {
			LOG_ERR("No CONNACK from broker");
			return -ETIMEDOUT;
		}

		if (mqtt_client_is_connected()) {
			if (birth_pending) {
				/* NCMD first, so a rebirth request can't be missed */
				rc = subscribe_ncmd();
				if (rc == 0) {
					rc = publish_nbirth();
				}
				if (rc) {
					LOG_ERR("NBIRTH failed: %d", rc);
					return rc;
				}
				birth_pending = false;
			}

			if (k_msgq_num_used_get(&sample_q) >= APP_PUBLISH_BATCH_SIZE) {
				rc = publish_ndata_batch();
				if (rc) {
					LOG_ERR("NDATA failed: %d", rc);
					return rc;
				}
			}
		}

		rc = zsock_poll(&fds, 1, 1000);
		if (rc < 0) {
			LOG_ERR("poll failed: %d", errno);
			return -errno;
		}

		if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
			LOG_ERR("socket error: revents=0x%x", fds.revents);
			return -ECONNRESET;
		}

		if (fds.revents & ZSOCK_POLLIN) {
			rc = mqtt_input(&client);
			if (rc < 0) {
				LOG_ERR("mqtt_input failed: %d", rc);
				return rc;
			}
		}

		rc = mqtt_live(&client);
		if (rc < 0 && rc != -EAGAIN) {
			LOG_ERR("mqtt_live failed: %d", rc);
			return rc;
		}
	}
}

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const char *node_id = build_client_id_get();
	int backoff_s = BACKOFF_MIN_S;

	sparkplug_build_topic(SP_MSG_NBIRTH, node_id, topic_nbirth, sizeof(topic_nbirth));
	sparkplug_build_topic(SP_MSG_NDEATH, node_id, topic_ndeath, sizeof(topic_ndeath));
	sparkplug_build_topic(SP_MSG_NDATA,  node_id, topic_ndata,  sizeof(topic_ndata));
	sparkplug_build_topic(SP_MSG_NCMD,   node_id, topic_ncmd,   sizeof(topic_ncmd));
	LOG_INF("Sparkplug node topics: %s", topic_ndata);

	/* Reconnect forever: Project B-style robustness also helps Project A */
	while (1) {
		atomic_set(&connected, 0);
		birth_pending = false;

		int rc = connect_mqtt();

		if (rc == 0) {
			rc = run_session();
			if (mqtt_client_is_connected()) {
				/* we got as far as CONNACK, so start over with short wait */
				backoff_s = BACKOFF_MIN_S;
			}
			mqtt_abort(&client);
		} else {
			LOG_ERR("mqtt_connect failed: %d", rc);
		}

		atomic_set(&connected, 0);
		LOG_INF("Reconnecting in %d s", backoff_s);
		k_sleep(K_SECONDS(backoff_s));
		backoff_s = MIN(backoff_s * 2, BACKOFF_MAX_S);
	}
}
