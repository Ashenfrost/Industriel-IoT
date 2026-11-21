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

/* On demand (APP_WIFI_ON_DEMAND): connect and wait for an IPv4 address,
 * at most timeout_s. Blocks light sleep until network_down(). Always call
 * network_down() afterwards, also when this fails. Harmless in always-on
 * mode (returns at once when already connected). */
int network_up(int timeout_s);

/* On demand: disconnect again and allow light sleep. */
void network_down(void);
