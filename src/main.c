#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/sys/printk.h>

#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

static struct net_mgmt_event_callback wifi_cb;
static struct net_if *iface;

static void handle_wifi_event(struct net_mgmt_event_callback *cb,
                              uint32_t mgmt_event,
                              struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    switch (mgmt_event) {
    case NET_EVENT_WIFI_CONNECT_RESULT:
        printk("Wi-Fi connected\n");
        break;

    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        printk("Wi-Fi disconnected\n");
        break;

    default:
        break;
    }
}

static int connect_wifi(void)
{
    struct wifi_connect_req_params cnx = {0};

    iface = net_if_get_default();
    if (!iface) {
        printk("No default network interface found\n");
        return -1;
    }

    net_mgmt_init_event_callback(&wifi_cb, handle_wifi_event,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    cnx.ssid = WIFI_SSID;
    cnx.ssid_length = strlen(WIFI_SSID);
    cnx.psk = WIFI_PASSWORD;
    cnx.psk_length = strlen(WIFI_PASSWORD);
    cnx.security = WIFI_SECURITY_TYPE_PSK;
    cnx.channel = WIFI_CHANNEL_ANY;
    cnx.mfp = WIFI_MFP_OPTIONAL;

    printk("Connecting to %s...\n", cnx.ssid);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &cnx, sizeof(cnx));
    if (ret) {
        printk("Wi-Fi connect failed: %d\n", ret);
        return ret;
    }

    return 0;
}

int main(void)
{
    printk("Starting Wi-Fi connect example\n");
    connect_wifi();
    return 0;
}
