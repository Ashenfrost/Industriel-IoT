#include "wifi_network.h"
#include "wifi_creds.h"
#include "app_config.h"
#include "power_mgmt.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/settings/settings.h>
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
static atomic_t wanted;     /* someone wants Wi-Fi up (always, or during a sample) */

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

/* Fast reconnect: the access point (channel + BSSID) of the last successful
 * connection, kept in flash, so a wake from deep sleep connects without
 * scanning every channel first. If it fails we clear it and scan as before.
 * DHCP is kept (a fixed IP could clash when the hotspot hands it out). */
struct ap_cache {
	uint8_t channel;
	uint8_t bssid[WIFI_MAC_ADDR_LEN];
};
static struct ap_cache known_ap;
static bool known_ap_valid;

static int ap_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (!settings_name_steq(name, "ap", &next) || next || len != sizeof(known_ap)) {
		return -ENOENT;
	}
	known_ap_valid = read_cb(cb_arg, &known_ap, sizeof(known_ap)) == sizeof(known_ap) &&
			 known_ap.channel != 0;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(wifi_ap, "wifi", NULL, ap_set, NULL, NULL);

static void ap_cache_use(bool use)
{
	if (use && known_ap_valid) {
		params.channel = known_ap.channel;
		memcpy(params.bssid, known_ap.bssid, sizeof(params.bssid));
	} else {
		params.channel = WIFI_CHANNEL_ANY;
		memset(params.bssid, 0, sizeof(params.bssid));
	}
}

/* After a connect: remember the AP if it changed (one flash write then) */
static void ap_cache_update(void)
{
	struct wifi_iface_status st = {0};
	struct ap_cache now;

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st)) != 0 ||
	    st.channel == 0) {
		return;
	}
	now.channel = st.channel;
	memcpy(now.bssid, st.bssid, sizeof(now.bssid));
	if (known_ap_valid && memcmp(&now, &known_ap, sizeof(now)) == 0) {
		return;
	}
	known_ap = now;
	known_ap_valid = true;
	if (settings_save_one("wifi/ap", &known_ap, sizeof(known_ap)) == 0) {
		LOG_INF("Wi-Fi AP saved for fast reconnect (channel %u)", known_ap.channel);
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
		/* Connected, or not wanted right now (on-demand mode between
		 * samples): nothing to do until something changes. */
		if (atomic_get(&wifi_up) || !atomic_get(&wanted)) {
			delay_s = RETRY_MIN_S;
			k_sem_take(&state_changed, K_FOREVER);
			continue;
		}

		k_sleep(K_SECONDS(delay_s));
		if (atomic_get(&wifi_up) || !atomic_get(&wanted)) {
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

int network_up(int timeout_s)
{
	int64_t deadline = k_uptime_get() + timeout_s * 1000LL;
	k_timeout_t until = K_TIMEOUT_ABS_MS(deadline);

	if (!APP_WIFI_ON_DEMAND) {
		/* always on: the supervisor keeps it up; just report */
		return network_is_connected() ? 0 : -ENOTCONN;
	}
	power_lock(PWR_WIFI);
	atomic_set(&wanted, 1);

	if (network_is_connected()) {
		return 0;
	}
	/* taken down by network_down() between samples */
	net_if_up(iface);
	k_sem_reset(&ipv4_ready_sem);
	/* First try the AP we know (no scan); after that a full scan */
	bool cached = known_ap_valid;

	ap_cache_use(cached);
	if (cached) {
		LOG_INF("Wi-Fi: known AP on channel %u", known_ap.channel);
	}
	/* Connect, and try again if an attempt fails at once (seen with phone
	 * hotspots right after a disconnect) until the deadline. No supervisor
	 * thread in on-demand mode, so nothing else competes for the link. */
	while (!atomic_get(&wifi_up)) {
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			LOG_WRN("Wi-Fi connect timeout");
			return -ETIMEDOUT;
		}
		k_sem_reset(&state_changed);
		if (request_connect() == 0) {
			/* the next connect or disconnect event, whichever comes */
			k_sem_take(&state_changed, K_MSEC(MIN(left, 10000)));
		}
		if (!atomic_get(&wifi_up) && cached) {
			/* hotspot moved to another channel, or a new AP: scan */
			LOG_INF("Wi-Fi: known AP not found, scanning");
			cached = false;
			known_ap_valid = false;
			ap_cache_use(false);
			continue;
		}
		if (!atomic_get(&wifi_up)) {
			k_sleep(K_MSEC(MIN(1000, MAX(deadline - k_uptime_get(), 0))));
		}
	}
	ap_cache_update();
	/* After a reconnect the old DHCP address may still be there, so no new
	 * "address added" event comes: check before waiting for one. */
	if (atomic_get(&ipv4_up) ||
	    net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL) {
		atomic_set(&ipv4_up, 1);
		return 0;
	}
	if (k_sem_take(&ipv4_ready_sem, until) != 0) {
		LOG_WRN("DHCP timeout");
		return -ETIMEDOUT;
	}
	return 0;
}

void network_down(void)
{
	if (!APP_WIFI_ON_DEMAND) {
		return; /* always on: stays connected */
	}
	atomic_set(&wanted, 0);
	if (atomic_get(&wifi_up)) {
		int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);

		if (ret && ret != -EALREADY) {
			LOG_WRN("Wi-Fi disconnect request failed: %d", ret);
		}
		/* wait (briefly) for the disconnect to finish before taking the
		 * interface down */
		for (int i = 0; i < 20 && atomic_get(&wifi_up); i++) {
			k_sleep(K_MSEC(100));
		}
	}
	/* Interface down between samples: the light-sleep build hung with the
	 * Wi-Fi driver up but disconnected. */
	net_if_down(iface);
	atomic_set(&ipv4_up, 0);
	power_unlock(PWR_WIFI);
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

	/* On demand: the interface stays down until the first network_up() */
	if (!APP_WIFI_ON_DEMAND) {
		net_if_up(iface);
	}

	params.ssid = WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = WIFI_PASSWORD;
	params.psk_length = strlen(WIFI_PASSWORD);
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.mfp = WIFI_MFP_OPTIONAL;

#if APP_WIFI_ON_DEMAND
	/* The AP of the last connection (fast reconnect, see ap_cache_use()) */
	if (settings_subsys_init() == 0) {
		(void)settings_load_subtree("wifi");
	}
	/* Wi-Fi stays off until a sample calls network_up(), which also does
	 * its own retries: no supervisor thread competing for the link. */
	LOG_INF("Wi-Fi on demand (SSID %s): connects for each sample only", params.ssid);
	ARG_UNUSED(supervisor_fn);
	return 0;
#endif

	k_thread_create(&supervisor_data, supervisor_stack,
			K_THREAD_STACK_SIZEOF(supervisor_stack), supervisor_fn,
			NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&supervisor_data, "wifi_supervisor");

	/* Always on: Wi-Fi is wanted for good, and no light sleep while it is
	 * (an associated ESP32 Wi-Fi link does not survive it). */
	atomic_set(&wanted, 1);
	power_lock(PWR_WIFI);
	LOG_INF("Connecting to Wi-Fi SSID: %s", params.ssid);

	/* The supervisor takes over from here, whether this first attempt
	 * works or not. */
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
