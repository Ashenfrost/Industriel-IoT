#include "display.h"
#include "simPLC.h"
#include "network.h"

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <U8g2lib.h>

// SH1106 128x64, full buffer, hardware I2C
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static bool displayOk = false;
static unsigned long lastDraw = 0;

void displayInit(void) {
  Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);

  // u8g2.begin() always reports success, so check for the display ourselves
  Wire.beginTransmission(DISPLAY_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("OLED not found, continuing without display");
    return;
  }

  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setFont(u8g2_font_6x10_tf);
  displayOk = true;

  displayShowMessage("IoTSimPLC", "Starting...");
}

void displayShowMessage(const char* line1, const char* line2) {
  if (!displayOk) return;
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, line1);
  u8g2.drawStr(0, 24, line2);
  u8g2.sendBuffer();
}

// Builds a list like "Top Mid" of the positions whose output is active
static bool buildList(char* buf, size_t size, bool humidity) {
  size_t len = 0;
  buf[0] = '\0';
  for (int i = 0; i < NUM_SENSORS; i++) {
    PlcOutput out = humidity ? outHum(i) : outTemp(i);
    if (simPlcGetOutput(out)) {
      len += snprintf(buf + len, size - len, "%s%.3s", len ? " " : "", simPlcPositionName(i));
    }
  }
  return len > 0;
}

void displayUpdate(void) {
  if (!displayOk) return;

  unsigned long now = millis();
  if (now - lastDraw < DISPLAY_REFRESH_MS) return;
  lastDraw = now;

  char line[32];

  u8g2.clearBuffer();

  // Network status
  if (network_is_connected()) {
    String ip = WiFi.localIP().toString();
    snprintf(line, sizeof line, "IP %s", ip.c_str());
  } else {
    snprintf(line, sizeof line, "No Wi-Fi");
  }
  u8g2.drawStr(0, 9, line);

  // One row per position, with HOT / HUM flags from the coils (max 21 chars)
  for (int i = 0; i < NUM_SENSORS; i++) {
    SensorReading r = simPlcGetInput(i);
    int n;
    if (r.valid) {
      n = snprintf(line, sizeof line, "%.3s %4.1fC %2.0f%%",
                   simPlcPositionName(i), r.temperature, r.humidity);
    } else {
      n = snprintf(line, sizeof line, "%.3s read error", simPlcPositionName(i));
    }
    if (simPlcGetOutput(outTemp(i))) n += snprintf(line + n, sizeof line - n, " HOT");
    if (simPlcGetOutput(outHum(i)))  n += snprintf(line + n, sizeof line - n, " HUM");

    u8g2.drawStr(0, 21 + 10 * i, line);
  }

  // Bottom: which positions are in alarm
  char list[24];
  bool anyTemp = buildList(list, sizeof list, false);
  if (anyTemp) {
    snprintf(line, sizeof line, "!! HOT: %s", list);
    u8g2.drawStr(0, 53, line);
  }

  bool anyHum = buildList(list, sizeof list, true);
  if (anyHum) {
    snprintf(line, sizeof line, "!! HUMID: %s", list);
    u8g2.drawStr(0, 62, line);
  }

  if (!anyTemp && !anyHum) {
    u8g2.drawStr(0, 53, "All OK");
  }

  u8g2.sendBuffer();
}
