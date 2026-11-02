#include "client_id.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <string.h>

static char client_id[32];

void build_client_id_initialize(void)
{
    struct net_if *iface = net_if_get_default();
    const struct net_linkaddr *lladdr;
    const uint8_t *mac;

    if (!iface) {
        snprintk(client_id, sizeof(client_id), "device-unknown");
        return;
    }

    lladdr = net_if_get_link_addr(iface);
    if (!lladdr || lladdr->len < 6) {
        snprintk(client_id, sizeof(client_id), "device-unknown");
        return;
    }

    mac = lladdr->addr;

    snprintk(client_id, sizeof(client_id),
             "device-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *build_client_id_get(void)
{
    return client_id;
}
