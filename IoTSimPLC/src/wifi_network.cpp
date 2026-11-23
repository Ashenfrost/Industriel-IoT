#include "wifi_network.h"
#include "credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define IPV4_TIMEOUT_MS         20000

// Set from the Wi-Fi event task, read from the main task
static volatile bool wifi_connected = false;
static volatile bool ipv4_ready     = false;

// Settings from network_initialize(), for network_maintain()
static char savedSsid[CRED_SSID_MAX];
static char savedPsk[CRED_PSK_MAX];
static bool haveSettings = false;

static void wifi_event_handler(arduino_event_id_t event, arduino_event_info_t info)
{
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("Wi-Fi connected");
      wifi_connected = true;
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("IPv4 address acquired: ");
      Serial.println(WiFi.localIP());
      ipv4_ready = true;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // Reason codes: 2xx are ESP-IDF's own, e.g. 201 = AP not found,
      // 202 = auth failed, 15/204 = 4-way handshake timeout (often a wrong
      // password or a security mode the AP doesn't accept)
      Serial.printf("Wi-Fi disconnected (reason %u)\n", info.wifi_sta_disconnected.reason);
      wifi_connected = false;
      ipv4_ready     = false;
      break;

    default:
      break;
  }
}

// Wait until *flag becomes true or the timeout expires. Returns true on success.
static bool wait_for(volatile bool *flag, unsigned long timeout_ms)
{
  unsigned long start = millis();
  while (!*flag) {
    if (millis() - start > timeout_ms) return false;
    delay(100);
  }
  return true;
}

int network_initialize(const char* ssid, const char* psk)
{
  WiFi.onEvent(wifi_event_handler);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("iotsimplc");
  WiFi.setAutoReconnect(true);   // reconnect on its own if the link drops
  WiFi.setSleep(false);          // no power-save, keeps Modbus latency low

  // Kept for network_maintain(), which reconnects with the same settings
  strlcpy(savedSsid, ssid, sizeof savedSsid);
  strlcpy(savedPsk, psk, sizeof savedPsk);
  haveSettings = true;

  // What the SimPLC sees of the network before it tries: signal, channel,
  // security. Makes "can't connect" problems visible on the serial port.
  int found = WiFi.scanNetworks();
  bool seen = false;
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == ssid) {
      seen = true;
      Serial.printf("Wi-Fi scan: found, RSSI %d dBm, channel %d, auth mode %d\n",
                    WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
    }
  }
  if (!seen) {
    Serial.printf("Wi-Fi scan: network not found (%d networks seen)\n", found);
  }
  WiFi.scanDelete();

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", ssid);

  // No WiFi.config() call, so the ESP32 uses DHCP (same as IoTEdge)
  WiFi.begin(ssid, psk[0] ? psk : nullptr);   // empty password = open network

  if (!wait_for(&wifi_connected, WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("Wi-Fi connect timeout");
    return -1;
  }

  if (!wait_for(&ipv4_ready, IPV4_TIMEOUT_MS)) {
    Serial.println("DHCP timeout");
    return -2;
  }

  Serial.println("Network is up");
  return 0;
}

bool network_is_connected(void)
{
  return wifi_connected && ipv4_ready;
}

// The Arduino core's auto-reconnect gives up after some failures: seen with
// the phone hotspot, which dropped the link (reason 3) right after
// connecting and then refused one attempt (reason 202). After that the
// SimPLC never tried again and stayed offline. So we retry ourselves.
#define RECONNECT_INTERVAL_MS 5000

void network_maintain(void)
{
  static unsigned long lastTry = 0;

  if (!haveSettings) return;  // no Wi-Fi settings: running without network
  if (network_is_connected()) return;
  if (millis() - lastTry < RECONNECT_INTERVAL_MS) return;
  lastTry = millis();

  Serial.println("Wi-Fi not up, reconnecting");
  WiFi.disconnect();
  WiFi.begin(savedSsid, savedPsk[0] ? savedPsk : nullptr);
}
