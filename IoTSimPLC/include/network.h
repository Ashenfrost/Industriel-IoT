// pragma once ensures that the header file is only included once
#pragma once

// Connects to the Wi-Fi network from wifi_creds.h and waits for a DHCP address.
// Returns 0 on success, -1 on Wi-Fi timeout, -2 on DHCP timeout.
int network_initialize(void);

// True while Wi-Fi is connected and an IPv4 address is held.
bool network_is_connected(void);
