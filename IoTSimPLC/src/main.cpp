#include <Arduino.h>
#include "simPLC.h"
#include "credentials.h"
#include "wifi_network.h"
#include "modbusTCP.h"
#include "alarm.h"
#include "display.h"
#include "buffer.h"

static void printReadings() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    SensorReading r = simPlcGetInput(i);
    if (r.valid) {
      Serial.printf("%-6s: Temperature = %.1f C, Humidity = %.1f %%\n",
                    simPlcPositionName(i), r.temperature, r.humidity);
    } else {
      Serial.printf("%-6s: read error\n", simPlcPositionName(i));
    }
  }
  Serial.printf("Heat alarms (top/mid/bot): %d%d%d | Humidity alarms: %d%d%d\n",
                simPlcGetOutput(OUT_TEMP_TOP), simPlcGetOutput(OUT_TEMP_MID),
                simPlcGetOutput(OUT_TEMP_BOT), simPlcGetOutput(OUT_HUM_TOP),
                simPlcGetOutput(OUT_HUM_MID), simPlcGetOutput(OUT_HUM_BOT));
  Serial.printf("heartbeat=%u status=0x%02X\n", simPlcHeartbeat(), simPlcStatus());
  Serial.printf("buffer: %u sample(s), seq %u..%u, oldest %lu s, clock %s\n\n",
                bufferCount(), bufferOldestSeq(), bufferNewestSeq(),
                (unsigned long)bufferOldestAgeS(), clockIsSet() ? "set" : "not set");
}

void setup() {
  Serial.begin(9600);
  simPlcInit();
  bufferInit();
  alarmInit();
  displayInit();

  char ssid[CRED_SSID_MAX];
  char psk[CRED_PSK_MAX];

  // Short window to change the stored Wi-Fi settings (hold BOOT after the board has started)
  displayShowMessage("Hold BOOT now to", "change Wi-Fi setup");
  if (credentialsResetRequested(3000)) {
    credentialsClear();
  }

  bool haveCreds = credentialsLoad(ssid, sizeof ssid, psk, sizeof psk);

  if (!haveCreds) {
    displayShowMessage("Wi-Fi setup needed", "Use serial monitor");
    if (credentialsProvisionSerial(120000)) {
      haveCreds = credentialsLoad(ssid, sizeof ssid, psk, sizeof psk);
    }
  }

  if (haveCreds) {
    displayShowMessage("IoTSimPLC", "Connecting Wi-Fi...");
    if (network_initialize(ssid, psk) != 0) {
      Serial.println("Network not ready, auto-reconnect will keep trying");
      Serial.println("Wrong password? Restart and hold BOOT to enter new settings");
    }
  } else {
    Serial.println("No Wi-Fi settings, running without network. Restart to try again");
  }

  modbusInit();
}

void loop() {
  simPlcUpdate();
  bufferUpdate();    // one sample every SAMPLE_INTERVAL_MS for the Edge
  network_maintain();
  modbusUpdate();   // must run every loop, so no delay() here
  alarmUpdate();
  displayUpdate();

  // Print only when a new scan has completed
  static uint16_t lastBeat = 0;
  if (simPlcHeartbeat() != lastBeat) {
    lastBeat = simPlcHeartbeat();
    printReadings();
  }
}
