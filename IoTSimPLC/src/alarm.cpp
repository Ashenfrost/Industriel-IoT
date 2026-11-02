#include "alarm.h"
#include "simPLC.h"

#include <Arduino.h>

void alarmInit(void) {
  pinMode(ALARM_TEMP_PIN, OUTPUT);
  pinMode(ALARM_HUM_PIN, OUTPUT);
  digitalWrite(ALARM_TEMP_PIN, LOW);
  digitalWrite(ALARM_HUM_PIN, LOW);
}

void alarmUpdate(void) {
  bool anyTemp = false;
  bool anyHum  = false;

  for (int i = 0; i < NUM_SENSORS; i++) {
    if (simPlcGetOutput(outTemp(i))) anyTemp = true;
    if (simPlcGetOutput(outHum(i)))  anyHum  = true;
  }

  // Non-blocking beep: on for one period, off for the next
  bool beepOn = ((millis() / BEEP_PERIOD_MS) % 2) == 0;

  digitalWrite(ALARM_TEMP_PIN, (anyTemp && beepOn) ? HIGH : LOW);
  digitalWrite(ALARM_HUM_PIN,  anyHum ? HIGH : LOW);
}
