// pragma once ensures that the header file is only included once
#pragma once

#include <stdbool.h>

/* Connects to the Wi-Fi from wifi_creds.h and waits (up to ~50 s) for an
 * IPv4 address. Returns 0 when the network is up. Either way it starts a
 * background thread that reconnects whenever Wi-Fi is down, so a negative
 * return only means "not up yet". */
int network_initialize(void);

/* True while Wi-Fi is connected and an IPv4 address is held. */
bool network_is_connected(void);
