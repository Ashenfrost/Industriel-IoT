/* mqtt_client.c - MQTT transport for the Sparkplug B edge node.
 *
 * "Option A": the node is only connected while it has something to send.
 * Every APP_PUBLISH_INTERVAL_S the MQTT thread runs one short session:
 *
 *   1. begin_session()                  -> new bdSeq, saved in flash
 *   2. CONNECT (clean session)
 *   3. CONNACK -> NBIRTH (seq 0)
 *   4. all queued samples as NDATA
 *   5. NDEATH (same bdSeq) published by us, then DISCONNECT
 *
 * Two uplinks, chosen with APP_UPLINK_CELLULAR in app_config.h:
 *
 *  - Wi-Fi: Zephyr's MQTT client over its own TCP/IP stack. NDEATH is also
 *    registered as the MQTT Will (QoS 1), so if the node dies mid-session the
 *    broker publishes it for us.
 *  - Cellular: the SIM7080G's built-in MQTT client over LTE-M (modem.c),
 *    optionally with TLS inside the modem. The modem only accepts the Will as
 *    a text string, so the binary NDEATH can't be the Will: a node that dies
 *    mid-session shows as online until its next NBIRTH. Sessions last a few
 *    seconds, so the window is small.
 *
 * Step 5 matters on both: a clean DISCONNECT tells the broker to throw any
 * Will away, so without our own NDEATH the host would think the node is
 * still online.
 *
 * The node does not subscribe to NCMD: it is offline most of the time, and
 * every session starts with a full NBIRTH anyway, which is what a rebirth
 * request would ask for.
 *
 * All MQTT, modem and sparkplug_* calls happen in this one thread. Other
 * threads only touch the sample queue.
 */
#include "mqtt_client.h"
#include "mqtt_settings.h"
#include "client_id.h"
#include "bdseq_store.h"
#include "time_sync.h"
#include "app_config.h"
#include "power_marker.h"
#include "power_mgmt.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#if APP_UPLINK_CELLULAR
#include "modem.h"
#include "mqtt_ca_cert.h"
/* mTLS (IEC 62443 CR 3.8): this device's client certificate and private key.
 * Secret, so it is not in git: make it from the PEM files with
 *   openssl rsa -in <device>.key -traditional -out <device>.pkcs1.key
 *   python tools/scripts/pem2h.py src/mqtt_client_cert.h \
 *       MQTT_CLIENT_CERT_PEM=<device>.crt MQTT_CLIENT_KEY_PEM=<device>.pkcs1.key
 * The SIM7080G needs the key as PKCS#1 ("BEGIN RSA PRIVATE KEY"). With a
 * PKCS#8 key ("BEGIN PRIVATE KEY") it sends a broken certificate and the
 * broker logs "asn1 ... sequence length mismatch".
 * The broker requires it on 8883 and uses its CN as the MQTT username. */
#if __has_include("mqtt_client_cert.h")
#include "mqtt_client_cert.h"
#elif APP_MQTT_TLS && !defined(APP_ALLOW_INSECURE)
#error "src/mqtt_client_cert.h missing: the broker requires a client certificate"
#endif
#ifndef MQTT_CLIENT_CERT_PEM
#define MQTT_CLIENT_CERT_PEM ""
#define MQTT_CLIENT_KEY_PEM  ""
#endif
#include <zephyr/device.h>
#else
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#endif

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_INF);

#define STACK_SIZE         4096
/* Sessions last seconds, but on Wi-Fi the keepalive also decides how fast
 * the broker notices a dead node (1.5 x keepalive) and publishes the Will. */
#define KEEPALIVE_S        60U

BUILD_ASSERT(APP_SAMPLE_QUEUE_LEN >= APP_PUBLISH_INTERVAL_S / APP_SAMPLE_INTERVAL_S,
	     "sample queue must hold at least one publish interval of samples");

static K_THREAD_STACK_DEFINE(mqtt_stack, STACK_SIZE);
static struct k_thread mqtt_thread_data;

K_MSGQ_DEFINE(sample_q, sizeof(struct sparkplug_sample), APP_SAMPLE_QUEUE_LEN, 8);

/* mqtt_client_send_now() <-> MQTT thread (deep sleep design) */
static K_SEM_DEFINE(send_req, 0, 1);
static K_SEM_DEFINE(send_done, 0, 1);
static volatile int send_result;
/* Given for every queued sample; only waited on in persistent mode */
static K_SEM_DEFINE(sample_ready, 0, K_SEM_MAX_LIMIT);
/* Counts samples dropped because the queue was full. The oldest samples are
 * the ones being published, so a drop during a publish means fewer items
 * are left to remove afterwards. */
static atomic_t samples_dropped;

/* Sparkplug topics, built once we know our node id */
static char topic_nbirth[SPARKPLUG_TOPIC_LEN];
static char topic_ndeath[SPARKPLUG_TOPIC_LEN];
static char topic_ndata[SPARKPLUG_TOPIC_LEN];

/* NDEATH of the current session. On Wi-Fi it is also the Will, so the
 * buffer must stay valid for the whole session. */
static uint8_t ndeath_buf[32];
static int ndeath_len;
static uint8_t payload_buf[SPARKPLUG_PAYLOAD_LEN];
static struct sparkplug_sample batch[SPARKPLUG_MAX_SAMPLES_PER_NDATA];

/* Last sample sent, so NBIRTH can report a current value */
static struct sparkplug_sample last_sample;
static bool have_last_sample;

static void mqtt_thread_fn(void *p1, void *p2, void *p3);

/* ------------------------------------------------------------------ */
/* Uplink: cellular (SIM7080G)                                         */
/* ------------------------------------------------------------------ */
#if APP_UPLINK_CELLULAR

/* AT+SMPUB takes at most 1024 bytes. One sample is SP_METRICS_PER_SAMPLE (7)
 * metrics of ~18 bytes = ~125 bytes, so 7 samples (~0.9 kB) fit with margin;
 * 8 (the metric limit) would be ~1 kB, too close. */
#define UPLINK_MAX_PAYLOAD      MODEM_MQTT_MAX_PAYLOAD
#ifndef APP_NDATA_SAMPLES_PER_MSG
#define APP_NDATA_SAMPLES_PER_MSG 7
#endif
#define NDATA_SAMPLES_PER_MSG   APP_NDATA_SAMPLES_PER_MSG

/* UTC before the TLS connect: extra tries after the first, TIME_RETRY_S apart */
#define TIME_RETRIES  5
#define TIME_RETRY_S  3

static struct modem_cfg modem_cfg;   /* must live forever (modem keeps a pointer) */
static bool modem_ready;

static int modem_time_source(int64_t *epoch_ms)
{
	return modem_get_utc_ms(APP_SNTP_SERVER, epoch_ms);
}

static int uplink_init(void)
{
	modem_cfg = (struct modem_cfg){
		.apn = APP_MODEM_APN,
		.broker = MQTT_BROKER_HOSTNAME,
		.port = APP_MQTT_TLS ? APP_MQTT_TLS_PORT : MQTT_BROKER_PORT,
		.client_id = build_client_id_get(),
		.username = MQTT_ESP32_USER,
		.password = MQTT_ESP32_PASS,
		.keepalive_s = KEEPALIVE_S,
		.clean_session = true,          /* Sparkplug requires clean session */
		.tls = APP_MQTT_TLS,
		.ca_cert_pem = MQTT_CA_CERT_PEM,
		.client_cert_pem = MQTT_CLIENT_CERT_PEM,
		.client_key_pem = MQTT_CLIENT_KEY_PEM,
	};
	/* Zephyr's SNTP can't reach the network through the modem */
	time_sync_set_source(modem_time_source);
	return 0;
}

/* Ends every session (clean or not): data connection down and, with
 * APP_MODEM_RADIO_OFF, the radio off until the next session. Then light
 * sleep is allowed again. */
static void uplink_shutdown(void)
{
	if (modem_ready) {
		(void)modem_pdp_down();
#if APP_MODEM_RADIO_OFF
		(void)modem_radio_off();
#endif
	}
	power_unlock(PWR_UPLINK);
}

static int uplink_open(void)
{
	int ret;

	/* UART traffic to the modem for the whole session: no light sleep */
	power_lock(PWR_UPLINK);

	if (!modem_ready) {
		/* modem_init() also turns the radio on (AT+CFUN=1) */
		ret = modem_init(DEVICE_DT_GET(DT_NODELABEL(uart1)), &modem_cfg);
		if (ret) {
			LOG_ERR("modem init failed: %d", ret);
			return ret;
		}
		modem_ready = true;
	} else if (APP_MODEM_RADIO_OFF) {
		ret = modem_radio_on();
		if (ret) {
			LOG_ERR("radio on failed: %d", ret);
			return ret;
		}
	}
	ret = modem_attach(APP_MODEM_ATTACH_MS);
	if (ret) {
		return ret;
	}
	ret = modem_pdp_up(APP_MODEM_APN);
	if (ret) {
		LOG_ERR("PDP up failed: %d", ret);
		return ret;
	}
	/* Time before connecting: NTP needs the data connection, and TLS needs a
	 * correct clock to check the certificate's validity dates. Right after
	 * the modem attaches neither NTP nor its own clock (NITZ) may be ready:
	 * after a cold start (every deep sleep wake) the first try failed with
	 * -11 and the TLS connect then failed. So retry for a few seconds. */
	time_sync_refresh_if_stale();
	for (int i = 0; i < TIME_RETRIES && !time_sync_is_valid(); i++) {
		k_sleep(K_SECONDS(TIME_RETRY_S));
		time_sync_refresh_if_stale();
	}
	if (!time_sync_is_valid()) {
		LOG_ERR("No valid time: TLS can't check the certificate, not connecting");
		return -ETIMEDOUT;
	}

	ret = modem_mqtt_connect();
	if (ret) {
		LOG_ERR("MQTT connect failed: %d", ret);
		return ret;
	}
	LOG_INF("MQTT connected");
	return 0;
}

/* Sparkplug: NBIRTH/NDATA/NDEATH are QoS 0 and never retained */
static int uplink_publish(const char *topic, const uint8_t *data, size_t len)
{
	power_marker_set(PM_MARK_PUBLISH, true);
	int ret = modem_mqtt_publish(topic, data, len, 0, false);

	power_marker_set(PM_MARK_PUBLISH, false);
	return ret;
}

static int uplink_close(void)
{
	int ret = modem_mqtt_disconnect();

	/* Option A: no data connection (and no radio) between sessions */
	uplink_shutdown();
	return ret;
}

static void uplink_abort(void)
{
	if (modem_ready) {
		/* (if the modem never answered there is nothing to tear down) */
		(void)modem_mqtt_disconnect();
	}
	uplink_shutdown();
}

/* ------------------------------------------------------------------ */
/* Uplink: Wi-Fi (Zephyr MQTT)                                         */
/* ------------------------------------------------------------------ */
#else

#define RX_BUF_SIZE        256
#define TX_BUF_SIZE        256
#define CONNACK_TIMEOUT_MS 10000
#define UPLINK_MAX_PAYLOAD      sizeof(payload_buf)
#define NDATA_SAMPLES_PER_MSG   SPARKPLUG_MAX_SAMPLES_PER_NDATA

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];

static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg;

/* Set from the event handler (called from mqtt_input in this thread) */
static bool connected;
static int connack_result;

static struct mqtt_utf8 user = {
	.utf8 = (uint8_t *)MQTT_ESP32_USER,
	.size = sizeof(MQTT_ESP32_USER) - 1,
};
static struct mqtt_utf8 pass = {
	.utf8 = (uint8_t *)MQTT_ESP32_PASS,
	.size = sizeof(MQTT_ESP32_PASS) - 1,
};

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
 * every session. Define it in mqtt_settings.h to turn it back on while
 * debugging. */
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

static int uplink_init(void)
{
	return 0; /* SNTP over Wi-Fi is the default time source */
}

static int uplink_open(void)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	const char *id = build_client_id_get();
	int ret;

	time_sync_refresh_if_stale();
	if (!time_sync_is_valid()) {
		LOG_WRN("No valid time: timestamps will be uptime-based");
	}

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

	/* The same NDEATH bytes are the Will and our own NDEATH at the end */
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
	/* TLS is only implemented for the cellular uplink (inside the modem) */
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	connected = false;
	connack_result = 0;
	ret = mqtt_connect(&client);
	if (ret) {
		LOG_ERR("mqtt_connect failed: %d", ret);
		return ret;
	}
	return wait_for_connack();
}

/* Sparkplug: NBIRTH/NDATA/NDEATH are QoS 0 and never retained */
static int uplink_publish(const char *topic, const uint8_t *data, size_t len)
{
	struct mqtt_publish_param param = {0};

	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.topic.topic.utf8 = (const uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.payload.data = (uint8_t *)data;
	param.message.payload.len = len;

	return mqtt_publish(&client, &param);
}

static int uplink_close(void)
{
	return mqtt_disconnect(&client, NULL);
}

static void uplink_abort(void)
{
	mqtt_abort(&client);
}

#endif /* APP_UPLINK_CELLULAR */

/* ------------------------------------------------------------------ */
/* Sparkplug session (same for both uplinks)                           */
/* ------------------------------------------------------------------ */

int mqtt_client_send_now(k_timeout_t timeout)
{
	k_sem_reset(&send_done);
	k_sem_give(&send_req);
	if (k_sem_take(&send_done, timeout) != 0) {
		return -EAGAIN;
	}
	return send_result;
}

int mqtt_client_queued(void)
{
	return (int)k_msgq_num_used_get(&sample_q);
}

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
	k_sem_give(&sample_ready);
	return 0;
}

static int publish_nbirth(void)
{
	int len = sparkplug_encode_nbirth(payload_buf, sizeof(payload_buf),
					  time_now_ms(),
					  have_last_sample ? &last_sample : NULL);
	if (len < 0) {
		return len;
	}

	int ret = uplink_publish(topic_nbirth, payload_buf, len);

	if (ret == 0) {
		LOG_INF("NBIRTH sent (%d bytes)", len);
	}
	return ret;
}

BUILD_ASSERT(NDATA_SAMPLES_PER_MSG <= SPARKPLUG_MAX_SAMPLES_PER_NDATA,
	     "more samples per message than the proto allows");

/* Sends one NDATA with up to NDATA_SAMPLES_PER_MSG queued samples.
 * Returns the number of samples sent (0 = queue empty) or a negative errno.
 * Samples are only removed from the queue once the publish succeeded. */
static int publish_ndata_chunk(void)
{
	size_t n = 0;
	atomic_val_t dropped_before = atomic_get(&samples_dropped);

	while (n < NDATA_SAMPLES_PER_MSG &&
	       k_msgq_peek_at(&sample_q, &batch[n], n) == 0) {
		n++;
	}
	if (n == 0) {
		return 0;
	}

	/* Queued samples carry uptime; convert to UTC with the latest sync */
	for (size_t i = 0; i < n; i++) {
		if (!batch[i].utc) {
			batch[i].timestamp_ms =
				time_uptime_to_epoch_ms((int64_t)batch[i].timestamp_ms);
		}
	}

	/* now_ms is the payload timestamp: when this message was sent */
	int len = sparkplug_encode_ndata(payload_buf, sizeof(payload_buf),
					 time_now_ms(), batch, n);

	if (len < 0) {
		return len;
	}
	/* Don't re-encode with fewer samples here: every encode uses up a seq
	 * number, and a gap makes the host ask for a rebirth. */
	if (len > (int)UPLINK_MAX_PAYLOAD) {
		LOG_ERR("NDATA %d bytes > uplink max %d: lower NDATA_SAMPLES_PER_MSG",
			len, (int)UPLINK_MAX_PAYLOAD);
		return -EMSGSIZE;
	}

	int ret = uplink_publish(topic_ndata, payload_buf, len);

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

/* Continues bdSeq from flash, so a reboot doesn't start over at 0 */
static void restore_bdseq(void)
{
	uint8_t last;
	int ret = bdseq_store_load(&last);

	if (ret == 0) {
		sparkplug_restore_bdseq(last);
		LOG_INF("bdSeq restored from flash: last %u", last);
	} else if (ret == -ENOENT) {
		LOG_INF("No saved bdSeq (new device), starting at 0");
	} else {
		LOG_ERR("bdSeq load failed (%d), starting at 0", ret);
	}
}

/* New session: new bdSeq, saved BEFORE connecting. If the node dies during
 * the session, the next one still gets a new bdSeq (CR 3.8). A failed
 * connect leaves a gap in the sequence, which the host allows. If the save
 * fails the session still runs; the host flags a repeated bdSeq. */
static void begin_session(void)
{
	sparkplug_begin_session();

	int ret = bdseq_store_save(sparkplug_get_bdseq());

	if (ret) {
		LOG_ERR("bdSeq %u not saved: %d", sparkplug_get_bdseq(), ret);
	}
}

/* One complete option A session. On any error the connection is aborted
 * and unsent samples stay queued. */
static int run_session(void)
{
	/* The same NDEATH bytes are used as the Will (Wi-Fi) and as our own
	 * NDEATH at the end of the session. */
	begin_session();
	ndeath_len = sparkplug_encode_ndeath(ndeath_buf, sizeof(ndeath_buf));
	if (ndeath_len < 0) {
		return ndeath_len;
	}

	power_marker_set(PM_MARK_SESSION, true);
	int rc = uplink_open();

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
		/* Leave cleanly: NDEATH ourselves, then DISCONNECT (drops any Will) */
		rc = uplink_publish(topic_ndeath, ndeath_buf, ndeath_len);
		if (rc == 0) {
			rc = uplink_close();
		}
		if (rc == 0) {
			LOG_INF("Session done, disconnected");
			power_marker_set(PM_MARK_SESSION, false);
			return 0;
		}
	}

	LOG_ERR("Session failed: %d", rc);
	uplink_abort();
	power_marker_set(PM_MARK_SESSION, false);
	return rc;
}

#if APP_PERSISTENT_SESSION
BUILD_ASSERT(APP_UPLINK_CELLULAR, "APP_PERSISTENT_SESSION is only implemented for cellular "
				  "(the modem keeps the MQTT keepalive by itself)");

/* "No power saving" mode for the baseline measurement: connect once, then
 * publish every sample as soon as it is queued. The SESSION marker stays
 * high while connected. On an error the connection is dropped and set up
 * again after APP_RETRY_INTERVAL_S. */
static void persistent_loop(void)
{
	bool up = false;

	while (1) {
		if (!up) {
			begin_session();
			ndeath_len = sparkplug_encode_ndeath(ndeath_buf, sizeof(ndeath_buf));
			power_marker_set(PM_MARK_SESSION, true);
			up = (uplink_open() == 0 && publish_nbirth() == 0);
			if (!up) {
				LOG_ERR("Persistent connect failed, retrying in %d s",
					APP_RETRY_INTERVAL_S);
				uplink_abort();
				power_marker_set(PM_MARK_SESSION, false);
				k_sleep(K_SECONDS(APP_RETRY_INTERVAL_S));
				continue;
			}
			LOG_INF("Persistent session up");
		}

		k_sem_take(&sample_ready, K_FOREVER);
		while (up && k_msgq_num_used_get(&sample_q) > 0) {
			if (publish_ndata_chunk() < 0) {
				LOG_ERR("Persistent publish failed, reconnecting");
				uplink_abort();
				power_marker_set(PM_MARK_SESSION, false);
				up = false;
			}
		}
	}
}
#endif

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
	LOG_INF("Uplink: %s%s", APP_UPLINK_CELLULAR ? "cellular (SIM7080G)" : "Wi-Fi",
		APP_UPLINK_CELLULAR && APP_MQTT_TLS ? " + TLS" : "");

	uplink_init();
	restore_bdseq();

#if APP_PERSISTENT_SESSION
	LOG_WRN("APP_PERSISTENT_SESSION: staying connected, no batching (power baseline)");
	persistent_loop();
#endif

#if APP_DEEP_SLEEP
	/* wake_cycle.c decides when to send; we only run the session */
	while (1) {
		k_sem_take(&send_req, K_FOREVER);
		int rc = run_session();

		send_result = (rc == 0 && k_msgq_num_used_get(&sample_q) == 0) ? 0 :
			      (rc ? rc : -EIO);
		k_sem_give(&send_done);
	}
#endif

	/* First session right away, so a fresh boot shows up on the host */
	int64_t next = k_uptime_get();

	while (1) {
		k_sleep(K_TIMEOUT_ABS_MS(next));

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
