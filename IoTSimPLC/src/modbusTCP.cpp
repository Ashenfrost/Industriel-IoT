#include "modbusTCP.h"
#include "simPLC.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ModbusIP_ESP8266.h>

// ---------- Register map (see modbusTCP.h) ----------
// Coil i drives PlcOutput i.
// Each sensor uses two holding registers: temperature at 2*i, humidity at 2*i + 1.
static const uint16_t HREG_HEARTBEAT = 2 * NUM_SENSORS;
static const uint16_t HREG_STATUS    = 2 * NUM_SENSORS + 1;

static ModbusIP mb;

// Value x10 as a Modbus register. Negative temperatures become int16 two's complement.
static uint16_t toRegister(float value) {
  return (uint16_t)(int16_t)lroundf(value * 10.0f);
}

void modbusInit(void) {
  mb.server();  // listen on port 502

  for (int i = 0; i < NUM_OUTPUTS; i++) {
    mb.addCoil(i, false);
  }

  for (int i = 0; i < 2 * NUM_SENSORS; i++) {
    mb.addHreg(i, 0);
  }
  mb.addHreg(HREG_HEARTBEAT, 0);

  // Until the first scan completes, report every sensor as in error
  mb.addHreg(HREG_STATUS, (1 << NUM_SENSORS) - 1);

  Serial.println("Modbus TCP server started on port 502");
}

void modbusUpdate(void) {
  mb.task();  // handle incoming Modbus requests

  // Coils -> outputs (only act when a coil actually changed)
  for (int i = 0; i < NUM_OUTPUTS; i++) {
    bool wanted = mb.Coil(i);
    if (wanted != simPlcGetOutput((PlcOutput)i)) {
      simPlcSetOutput((PlcOutput)i, wanted);
      Serial.printf("Coil %d -> output %s\n", i, wanted ? "ON" : "OFF");
    }
  }

  // Inputs -> registers (only after a new scan)
  static uint16_t lastBeat = 0;
  uint16_t beat = simPlcHeartbeat();
  if (beat != lastBeat) {
    lastBeat = beat;

    for (int i = 0; i < NUM_SENSORS; i++) {
      // Failed reads keep the last good value, the status bit tells the Edge
      SensorReading r = simPlcGetInput(i);
      mb.Hreg(2 * i,     toRegister(r.temperature));
      mb.Hreg(2 * i + 1, toRegister(r.humidity));
    }
    mb.Hreg(HREG_HEARTBEAT, beat);
    mb.Hreg(HREG_STATUS, simPlcStatus());
  }
}
