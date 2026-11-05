#include <DHT.h>
#include "simPLC.h"

static DHT sensors[NUM_SENSORS] = {
  DHT(SENSOR_PINS[0], DHT_TYPE),
  DHT(SENSOR_PINS[1], DHT_TYPE),
  DHT(SENSOR_PINS[2], DHT_TYPE)
};

static const char* POSITION_NAMES[NUM_SENSORS] = {"Top", "Middle", "Bottom"};

// Input image: latest sensor readings
static SensorReading inputs[NUM_SENSORS];

// Output image: what IoTEdge has commanded (no pins here, see alarm.cpp)
static bool outputs[NUM_OUTPUTS];

static uint16_t heartbeat = 0;
static uint16_t status    = 0;
static unsigned long lastScan = 0;

void simPlcInit() {
  for (int i = 0; i < NUM_OUTPUTS; i++) {
    outputs[i] = false;
  }

  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].begin();
    inputs[i] = {0.0f, 0.0f, false};
  }

  // First scan happens after one interval, so the DHT11s have time to power up
  lastScan = millis();
}

void simPlcUpdate() {
  unsigned long now = millis();
  if (now - lastScan < SCAN_INTERVAL_MS) return;
  lastScan = now;

  uint16_t newStatus = 0;

  for (int i = 0; i < NUM_SENSORS; i++) {
    float t = sensors[i].readTemperature();
    float h = sensors[i].readHumidity();

    if (isnan(t) || isnan(h)) {
      // Keep the last good values, but flag the sensor as invalid
      inputs[i].valid = false;
      newStatus |= (1 << i);
    } else {
      inputs[i] = {t, h, true};
    }
  }

  status = newStatus;
  heartbeat++;
}

SensorReading simPlcGetInput(int index) {
  if (index < 0 || index >= NUM_SENSORS) return {0.0f, 0.0f, false};
  return inputs[index];
}

uint16_t simPlcHeartbeat() { return heartbeat; }
uint16_t simPlcStatus()    { return status; }

const char* simPlcPositionName(int index) {
  if (index < 0 || index >= NUM_SENSORS) return "?";
  return POSITION_NAMES[index];
}

void simPlcSetOutput(PlcOutput out, bool state) {
  if (out < 0 || out >= NUM_OUTPUTS) return;
  outputs[out] = state;
}

bool simPlcGetOutput(PlcOutput out) {
  if (out < 0 || out >= NUM_OUTPUTS) return false;
  return outputs[out];
}
