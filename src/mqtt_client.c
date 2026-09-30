/* mqtt_client.c */
#include "mqtt_client.h"
#include "mqtt_settings.h"
#include "client_id.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_INF);

#define RX_BUF_SIZE  256
#define TX_BUF_SIZE  256
#define STACK_SIZE   2048

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buffer[RX_BUF_SIZE];
static uint8_t tx_buffer[TX_BUF_SIZE];
static struct zsock_pollfd fds;

static K_THREAD_STACK_DEFINE(mqtt_stack, STACK_SIZE);
static struct k_thread mqtt_thread_data;

static int connect_mqtt(void);
static int mqtt_get_socket(struct mqtt_client *c);
static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt);
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

int mqtt_client_publish_payload(const char *payload)
{
	struct mqtt_publish_param param = {0};

	param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	param.message.topic.topic.utf8 = (uint8_t *)MQTT_PUBLISH_TOPIC;
	param.message.topic.topic.size = strlen(MQTT_PUBLISH_TOPIC);
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = strlen(payload);

	return mqtt_publish(&client, &param);
}

static int mqtt_get_socket(struct mqtt_client *c)
{
	return c->transport.tcp.sock;
}

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
    addr.sin_port = htons(1883);

    ret = zsock_inet_pton(AF_INET, "193.181.218.152", &addr.sin_addr);
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

	ret = test_raw_tcp_socket();
    if (ret < 0) {
        return ret;
    }

	mqtt_client_init(&client);

	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (uint8_t *)id;
	client.client_id.size = strlen(id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.keepalive = 60U;
	client.clean_session = 1;
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	return mqtt_connect(&client);
}

static void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		LOG_INF("MQTT connected");
		mqtt_client_publish_payload("hello world");
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT disconnected");
		break;
	default:
		break;
	}
}

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int rc = connect_mqtt();
	if (rc) {
		LOG_ERR("mqtt_connect failed: %d", rc);
		return;
	}

	fds.fd = mqtt_get_socket(&client);
	fds.events = ZSOCK_POLLIN;

	while (1) {
		rc = zsock_poll(&fds, 1, 1000);
		if (rc < 0) {
			LOG_ERR("poll failed: %d", rc);
			break;
		}

		if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
			LOG_ERR("socket error: revents=0x%x", fds.revents);
			break;
		}

		if (fds.revents & ZSOCK_POLLIN) {
			rc = mqtt_input(&client);
			if (rc < 0) {
				LOG_ERR("mqtt_input failed: %d", rc);
				break;
			}
		}

		rc = mqtt_live(&client);
		if (rc < 0 && rc != -EAGAIN) {
			LOG_ERR("mqtt_live failed: %d", rc);
			break;
		}
	}

	mqtt_abort(&client);
}
