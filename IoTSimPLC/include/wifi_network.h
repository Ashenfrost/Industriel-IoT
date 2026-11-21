// pragma once ensures that the header file is only included once
#pragma once

// Connects to the given Wi-Fi network and waits for a DHCP address.
// Pass an empty psk for an open network.
// Returns 0 on success, -1 on Wi-Fi timeout, -2 on DHCP timeout.
int network_initialize(const char* ssid, const char* psk);

// True while Wi-Fi is connected and an IPv4 address is held.
bool network_is_connected(void);
