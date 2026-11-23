#pragma once

#include <stddef.h>

// Wi-Fi credentials stored in NVS (flash), so nothing secret lives in the code or in git.

// Buffer sizes: SSID max 32 chars, WPA2 passphrase max 63 chars, plus the terminating 0
#define CRED_SSID_MAX 33
#define CRED_PSK_MAX  65

// The BOOT button on the ESP32 board (GPIO0, low while pressed)
#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif

// Load the stored SSID and password. Returns false if nothing valid is stored.
// An empty password is valid (open network).
bool credentialsLoad(char* ssid, size_t ssidLen, char* psk, size_t pskLen);

// Store SSID and password. Returns false if they are invalid or could not be written.
bool credentialsSave(const char* ssid, const char* psk);

// Delete the stored credentials.
void credentialsClear(void);

// Ask for SSID and password in the serial monitor and store them.
// Gives up after timeoutMs without any typing. Returns true if saved.
bool credentialsProvisionSerial(unsigned long timeoutMs);

// Gives the user windowMs milliseconds to hold the BOOT button (GPIO 0).
// Returns true if it was pressed. Hold it AFTER the board has started, not while
// pressing reset, because BOOT held during reset puts the ESP32 in download mode.
bool credentialsResetRequested(unsigned long windowMs);
