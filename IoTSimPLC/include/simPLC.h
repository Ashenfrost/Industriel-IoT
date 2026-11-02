#ifndef SIMPLC_H
#define SIMPLC_H

#include <Arduino.h>

// ---------- Configuration ----------

// Sensors (index 0 = top, 1 = middle, 2 = bottom)
#define NUM_SENSORS 3
#define DHT_TYPE DHT11

// DHT11 data pins (GPIO numbers, safe on esp-wrover-kit)
const uint8_t SENSOR_PINS[NUM_SENSORS] = {25, 26, 27};

// Scan cycle (DHT11 needs at least 1 s between reads)
const unsigned long SCAN_INTERVAL_MS = 2500;

// ---------- Output image ----------
// Logical outputs = the Modbus coils, written by IoTEdge (the decision is made there).
// The physical actuators are driven from this image by alarm.cpp and display.cpp.
enum PlcOutput {
  OUT_TEMP_TOP = 0, OUT_TEMP_MID, OUT_TEMP_BOT,  // overheating, per position
  OUT_HUM_TOP,      OUT_HUM_MID,  OUT_HUM_BOT,   // too humid, per position
  NUM_OUTPUTS
};
static_assert(NUM_OUTPUTS == 2 * NUM_SENSORS, "one temp and one humidity output per sensor");

// Output for a given sensor index 0..NUM_SENSORS-1
inline PlcOutput outTemp(int sensor) { return (PlcOutput)(OUT_TEMP_TOP + sensor); }
inline PlcOutput outHum(int sensor)  { return (PlcOutput)(OUT_HUM_TOP + sensor); }

// ---------- Interface ----------

// One reading from one sensor
struct SensorReading {
  float temperature;  // degrees C
  float humidity;     // percent
  bool  valid;        // false if the last read failed (old values are kept)
};

// Set up sensors (outputs start OFF)
void simPlcInit();

// Call every loop(). Non-blocking: it only scans when SCAN_INTERVAL_MS has passed.
void simPlcUpdate();

// ----- Input image -----
SensorReading simPlcGetInput(int index);  // sensor 0..NUM_SENSORS-1
uint16_t simPlcHeartbeat();               // increments once per scan
uint16_t simPlcStatus();                  // bit i = sensor i read error
const char* simPlcPositionName(int index);  // "Top", "Middle", "Bottom"

// ----- Output image -----
void simPlcSetOutput(PlcOutput out, bool state);
bool simPlcGetOutput(PlcOutput out);

#endif
