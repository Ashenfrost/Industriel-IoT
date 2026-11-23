#include "buffer.h"

// Ring buffer: `head` is the index of the oldest sample, `count` how many
// samples are stored. Index of the k-th oldest = (head + k) % BUFFER_SIZE.
static Sample   ring[BUFFER_SIZE];
static uint16_t head    = 0;
static uint16_t count   = 0;
static uint16_t nextSeq = 1;
static uint32_t dropped = 0;
static uint16_t lastAck = 0;
static unsigned long lastSample = 0;

static bool     clockValid = false;
static uint32_t clockUtcAtSet = 0;  // UTC seconds written by the Edge
static uint32_t clockMsAtSet  = 0;  // millis() at that moment

// Value x10 as int16, like the live registers (modbusTCP.cpp)
static int16_t x10(float value) {
  return (int16_t)lroundf(value * 10.0f);
}

void bufferInit() {
  head = count = 0;
  nextSeq = 1;
  lastSample = millis();
}

static void push(const Sample& s) {
  if (count == BUFFER_SIZE) {
    head = (head + 1) % BUFFER_SIZE;  // full: drop the oldest
    count--;
    dropped++;
  }
  ring[(head + count) % BUFFER_SIZE] = s;
  count++;
}

void bufferUpdate() {
  unsigned long now = millis();
  if (now - lastSample < SAMPLE_INTERVAL_MS) return;
  if (simPlcHeartbeat() == 0) return;  // no scan yet, nothing to store
  lastSample = now;

  Sample s;
  s.seq = nextSeq;
  nextSeq = (nextSeq == 65535) ? 1 : nextSeq + 1;  // 0 means "none"
  s.ms = now;
  s.status = simPlcStatus();
  for (int i = 0; i < NUM_SENSORS; i++) {
    // Failed reads keep the last good value; the status bit flags them
    SensorReading r = simPlcGetInput(i);
    s.temp_x10[i] = x10(r.temperature);
    s.hum_x10[i]  = x10(r.humidity);
  }
  push(s);
}

uint16_t bufferCount()     { return count; }
uint16_t bufferOldestSeq() { return count ? ring[head].seq : 0; }
uint16_t bufferNewestSeq() { return count ? ring[(head + count - 1) % BUFFER_SIZE].seq : 0; }
uint32_t bufferDropped()   { return dropped; }
uint16_t bufferLastAck()   { return lastAck; }

uint32_t bufferOldestAgeS() {
  return count ? (millis() - ring[head].ms) / 1000 : 0;
}

// Position (0 = oldest) of the sample with this seq, or -1
static int findSeq(uint16_t seq) {
  if (seq == 0) return -1;
  for (uint16_t k = 0; k < count; k++) {
    if (ring[(head + k) % BUFFER_SIZE].seq == seq) return k;
  }
  return -1;
}

const Sample* bufferGet(uint16_t start, uint16_t k) {
  int pos = findSeq(start);
  if (pos < 0 || pos + k >= count) return nullptr;
  return &ring[(head + pos + k) % BUFFER_SIZE];
}

void bufferAck(uint16_t seq) {
  int pos = findSeq(seq);
  if (pos < 0) return;
  head = (head + pos + 1) % BUFFER_SIZE;
  count -= pos + 1;
  lastAck = seq;
}

void clockSet(uint32_t utc_s) {
  clockUtcAtSet = utc_s;
  clockMsAtSet = millis();
  clockValid = (utc_s != 0);
}

bool clockIsSet() { return clockValid; }

uint32_t clockAt(uint32_t ms) {
  if (!clockValid) return 0;
  // Signed: samples taken before the clock was set are in the past
  int32_t diffMs = (int32_t)(ms - clockMsAtSet);
  return clockUtcAtSet + diffMs / 1000;
}

uint32_t clockNow() { return clockAt(millis()); }
