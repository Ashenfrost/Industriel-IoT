#pragma once

// Sample buffer and clock for the Edge (batching on the SimPLC).
//
// The Edge sleeps (deep sleep) most of the time, so the SimPLC takes the
// samples itself: every SAMPLE_INTERVAL_MS the latest scan goes into a ring
// buffer together with a sequence number and the time it was taken. When
// the Edge wakes up it sets our clock, reads the buffer over Modbus and,
// once the data has reached the cloud, acknowledges it (see modbusTCP.h).
// Only acknowledged samples are removed, so a failed upload loses nothing.
//
// Clock: the Edge writes UTC (it gets it from the cellular network). We keep
// the offset to millis(), so a sample's UTC is computed when it is read.
// Samples taken before the clock was set get the right time too, as long as
// the SimPLC hasn't rebooted. Without a set clock the time is 0, and the Edge
// uses its own read time instead.

#include <Arduino.h>
#include "simPLC.h"

// One sample every 10 s, two hours of room: the Edge sends once an hour, so
// one failed upload still fits. When full, the oldest sample is dropped.
const unsigned long SAMPLE_INTERVAL_MS = 10000;
const uint16_t BUFFER_SIZE = 720;

struct Sample {
  uint16_t seq;                    // 1..65535, wraps and skips 0
  uint32_t ms;                     // millis() when taken
  uint16_t status;                 // bit i = sensor i read error
  int16_t  temp_x10[NUM_SENSORS];  // degrees C x10
  int16_t  hum_x10[NUM_SENSORS];   // percent x10
};

void bufferInit();
// Call every loop(). Takes a sample when SAMPLE_INTERVAL_MS has passed and at
// least one scan has completed.
void bufferUpdate();

uint16_t bufferCount();
uint16_t bufferOldestSeq();   // 0 if empty
uint16_t bufferNewestSeq();   // 0 if empty
uint32_t bufferOldestAgeS();  // 0 if empty
uint32_t bufferDropped();     // samples lost because the buffer was full
uint16_t bufferLastAck();

// k-th sample from the one with sequence number `start` (k = 0 is `start`).
// nullptr if `start` is not buffered or there are fewer samples.
const Sample* bufferGet(uint16_t start, uint16_t k);

// Removes every sample up to and including `seq`. Ignored if `seq` is not
// buffered (already removed, or never existed).
void bufferAck(uint16_t seq);

// ----- Clock -----
void     clockSet(uint32_t utc_s);
bool     clockIsSet();
uint32_t clockNow();            // UTC seconds, 0 if not set
uint32_t clockAt(uint32_t ms);  // UTC seconds for a millis() value, 0 if not set
