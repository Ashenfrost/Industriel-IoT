#include "network.h"
#include "wifi_creds.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(network, LOG_LEVEL_INF);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_sem wifi_connected_sem;
static struct k_sem ipv4_ready_sem;

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (status && status->status) {
			LOG_ERR("Wi-Fi connect failed: %d", status->status);
		} else {
			LOG_INF("Wi-Fi connected");
			k_sem_give(&wifi_connected_sem);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_INF("Wi-Fi disconnected");
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address acquired");
		k_sem_give(&ipv4_ready_sem);
	}
}

int network_init(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params params = { 0 };
	int ret;

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

	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("net_mgmt connect request failed: %d", ret);
		return ret;
	}

	ret = k_sem_take(&wifi_connected_sem, K_SECONDS(10));
	if (ret) {
		LOG_ERR("Wi-Fi connect timeout");
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
