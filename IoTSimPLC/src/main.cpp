#include <Arduino.h>
#include "simPLC.h"
#include "network.h"
#include "modbusTCP.h"
#include "alarm.h"
#include "display.h"

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
  Serial.printf("heartbeat=%u status=0x%02X\n\n", simPlcHeartbeat(), simPlcStatus());
}

void setup() {
  Serial.begin(9600);
  simPlcInit();
  alarmInit();
  displayInit();

  displayShowMessage("IoTSimPLC", "Connecting Wi-Fi...");
  if (network_initialize() != 0) {
    Serial.println("Network not ready, auto-reconnect will keep trying");
  }

  modbusInit();
}

void loop() {
  simPlcUpdate();
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
