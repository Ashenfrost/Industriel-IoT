#include "network.h"
#include "wifi_creds.h"

#include <Arduino.h>
#include <WiFi.h>

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define IPV4_TIMEOUT_MS         20000

// Set from the Wi-Fi event task, read from the main task
static volatile bool wifi_connected = false;
static volatile bool ipv4_ready     = false;

static void wifi_event_handler(arduino_event_id_t event)
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
      Serial.println("Wi-Fi disconnected");
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

int network_initialize(void)
{
  WiFi.onEvent(wifi_event_handler);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("iotsimplc");
  WiFi.setAutoReconnect(true);   // reconnect on its own if the link drops
  WiFi.setSleep(false);          // no power-save, keeps Modbus latency low

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);

  // No WiFi.config() call, so the ESP32 uses DHCP (same as IoTEdge)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
