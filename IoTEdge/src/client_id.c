/* client_id.c - unique MQTT client ID / Sparkplug edge node ID per device.
 *
 * IEC 62443 CR 3.8 (unique session identifiers): two devices with the same
 * client ID would kick each other off the broker and mix their data. The ID
 * is the chip's base MAC address, read with Zephyr's hwinfo API straight
 * from eFuse (one-time programmed at the factory). Unlike the MAC of the
 * Wi-Fi interface it doesn't depend on the network stack being up. On the
 * ESP32-C6 the Wi-Fi station MAC is the base MAC, so the ID is unchanged.
 *
 * There is no fallback ID: if the MAC can't be read, the caller must not
 * start MQTT.
 */
#include "client_id.h"

#include <errno.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

static char client_id[32];

int build_client_id_initialize(void)
{
	uint8_t mac[6];
	ssize_t n = hwinfo_get_device_id(mac, sizeof(mac));

	client_id[0] = '\0';
	if (n < 0) {
		return (int)n;
	}
	if (n != sizeof(mac)) {
		return -EIO;
	}

	/* All 0x00 or all 0xFF means unprogrammed eFuse, not a real MAC */
	bool all_zero = true;
	bool all_ff = true;

	for (size_t i = 0; i < sizeof(mac); i++) {
		all_zero &= (mac[i] == 0x00);
		all_ff &= (mac[i] == 0xFF);
	}
	if (all_zero || all_ff) {
		return -ENODATA;
	}

	snprintk(client_id, sizeof(client_id),
		 "device-%02x%02x%02x%02x%02x%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return 0;
}

const char *build_client_id_get(void)
{
	return client_id;
}
