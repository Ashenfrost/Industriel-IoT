#pragma once

// ---------- Configuration ----------

// Actuator pins (same pins as the old alarm outputs)
const int ALARM_TEMP_PIN = 32;   // buzzer: beeps while ANY position is overheating
const int ALARM_HUM_PIN  = 33;   // steady on while ANY position is too humid

// Beep pattern for the temperature alarm (active buzzer assumed)
const unsigned long BEEP_PERIOD_MS = 250;

// ---------- Interface ----------

// Set up the actuator pins (both OFF)
void alarmInit(void);

// Call every loop(). Drives the pins from the PLC output image (the coils).
void alarmUpdate(void);
