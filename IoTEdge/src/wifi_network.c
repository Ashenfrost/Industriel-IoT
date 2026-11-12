#include "wifi_network.h"
#include "wifi_creds.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(network, LOG_LEVEL_INF);

/* Reconnect back-off: 5, 10, 20, 40, 60, 60 ... s. Long enough not to hammer
 * an access point that is gone (or one that just kicked us), short enough
 * that Modbus to the SimPLC comes back within a minute. */
#define RETRY_MIN_S         5
#define RETRY_MAX_S         60
/* Time to let one connect attempt finish (association + 4-way handshake) */
#define CONNECT_WAIT_S      15
#define SUPERVISOR_STACK    2048

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_sem wifi_connected_sem;
static struct k_sem ipv4_ready_sem;
static K_SEM_DEFINE(state_changed, 0, 1);

static atomic_t wifi_up;    /* associated with the access point */
static atomic_t ipv4_up;    /* and holding a DHCP address */

static struct net_if *iface;
static struct wifi_connect_req_params params;

static K_THREAD_STACK_DEFINE(supervisor_stack, SUPERVISOR_STACK);
static struct k_thread supervisor_data;

/* Runs in the net_mgmt thread: only record the state and wake the
 * supervisor; never call net_mgmt() from here. */
static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (status && status->status) {
			LOG_ERR("Wi-Fi connect failed: %d", status->status);
			atomic_set(&wifi_up, 0);
		} else {
			LOG_INF("Wi-Fi connected");
			atomic_set(&wifi_up, 1);
			k_sem_give(&wifi_connected_sem);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_INF("Wi-Fi disconnected");
		atomic_set(&wifi_up, 0);
		atomic_set(&ipv4_up, 0);
	}
	k_sem_give(&state_changed);
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address acquired");
		atomic_set(&ipv4_up, 1);
		k_sem_give(&ipv4_ready_sem);
	}
}

static int request_connect(void)
{
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

	if (ret == -EALREADY || ret == -EINPROGRESS) {
		return 0; /* an attempt is already running */
	}
	if (ret) {
		LOG_ERR("net_mgmt connect request failed: %d", ret);
	}
	return ret;
}

/* Keeps Wi-Fi up: when the connection is lost (or never came up), it tries
 * again with a growing pause. Without it, one failed attempt at boot meant
 * no Modbus until the next reset. */
static void supervisor_fn(void *p1, void *p2, void *p3)
{
	int delay_s = RETRY_MIN_S;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		if (atomic_get(&wifi_up)) {
			delay_s = RETRY_MIN_S;
			k_sem_take(&state_changed, K_FOREVER); /* until a disconnect */
			continue;
		}

		k_sleep(K_SECONDS(delay_s));
		if (atomic_get(&wifi_up)) {
			continue;
		}

		LOG_INF("Wi-Fi down, reconnecting (next pause %d s)", MIN(delay_s * 2, RETRY_MAX_S));
		(void)request_connect();
		k_sleep(K_SECONDS(CONNECT_WAIT_S));
		/* Events from this attempt are already reflected in wifi_up */
		k_sem_reset(&state_changed);
		delay_s = MIN(delay_s * 2, RETRY_MAX_S);
	}
}

bool network_is_connected(void)
{
	return atomic_get(&wifi_up) && atomic_get(&ipv4_up);
}

int network_initialize(void)
{
	int ret;

	iface = net_if_get_default();
	k_sem_init(&wifi_connected_sem, 0, 1);
	k_sem_init(&ipv4_ready_sem, 0, 1);

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	net_if_up(iface);

	params.ssid = WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = WIFI_PASSWORD;
	params.psk_length = strlen(WIFI_PASSWORD);
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.mfp = WIFI_MFP_OPTIONAL;

	LOG_INF("Connecting to Wi-Fi SSID: %s", params.ssid);

	/* The supervisor takes over from here, whether this first attempt
	 * works or not. */
	k_thread_create(&supervisor_data, supervisor_stack,
			K_THREAD_STACK_SIZEOF(supervisor_stack), supervisor_fn,
			NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&supervisor_data, "wifi_supervisor");

	ret = request_connect();
	if (ret) {
		return ret;
	}

	ret = k_sem_take(&wifi_connected_sem, K_SECONDS(30));
	if (ret) {
		LOG_ERR("Wi-Fi connect timeout (will keep retrying)");
		return ret;
	}

	ret = k_sem_take(&ipv4_ready_sem, K_SECONDS(20));
	if (ret) {
		LOG_ERR("DHCP timeout");
		return ret;
	}

	LOG_INF("Network is up");
	return 0;
}
