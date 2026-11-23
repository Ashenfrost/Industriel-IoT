// PC test of the SimPLC sample buffer and clock (src/buffer.cpp).
//
//   g++ -std=c++17 -I test/native -I include test/native/test_buffer.cpp src/buffer.cpp -o test_buffer
//   ./test_buffer
//
// Arduino's millis() and the sensor scan are faked, so time can be stepped.
#include "buffer.h"

unsigned long fake_millis = 0;
static uint16_t fake_beat = 1;
static SensorReading fake_reading = {22.5f, 40.0f, true};

uint16_t simPlcHeartbeat() { return fake_beat; }
uint16_t simPlcStatus() { return 0; }
SensorReading simPlcGetInput(int) { return fake_reading; }

static int failures = 0;
static void check(const char* what, bool ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// Advance time by `n` sample intervals, running the loop each step
static void run(int n) {
  for (int i = 0; i < n; i++) {
    fake_millis += SAMPLE_INTERVAL_MS;
    bufferUpdate();
  }
}

int main() {
  fake_millis = 1000;
  bufferInit();

  fake_beat = 0;
  run(1);
  check("no sample before the first scan", bufferCount() == 0);
  fake_beat = 1;

  run(5);
  check("5 samples, seq 1..5", bufferCount() == 5 && bufferOldestSeq() == 1 &&
                                    bufferNewestSeq() == 5);
  check("values stored x10", bufferGet(1, 0)->temp_x10[0] == 225 &&
                                  bufferGet(1, 0)->hum_x10[2] == 400);
  check("oldest age = 40 s", bufferOldestAgeS() == 40);

  // Clock set after the samples were taken: they still get the right UTC
  uint32_t utc_now = 1795000000;
  clockSet(utc_now);
  check("clock set", clockIsSet() && clockNow() == utc_now);
  check("sample 5 taken now", clockAt(bufferGet(5, 0)->ms) == utc_now);
  check("sample 1 taken 40 s before the clock was set",
        clockAt(bufferGet(1, 0)->ms) == utc_now - 40);

  // Window access
  check("window k=2 from seq 2 is seq 4", bufferGet(2, 2)->seq == 4);
  check("past the end is empty", bufferGet(2, 4) == nullptr);
  check("unknown start is empty", bufferGet(99, 0) == nullptr);

  // Acknowledge
  bufferAck(3);
  check("ack 3 removes 1..3", bufferCount() == 2 && bufferOldestSeq() == 4 &&
                                   bufferLastAck() == 3);
  bufferAck(2);  // already gone
  check("ack of a removed seq is ignored", bufferCount() == 2);
  bufferAck(0);
  check("ack 0 is ignored", bufferCount() == 2);

  // Full buffer drops the oldest
  run(BUFFER_SIZE);
  check("full: count = BUFFER_SIZE", bufferCount() == BUFFER_SIZE);
  check("full: 2 dropped", bufferDropped() == 2);
  check("full: oldest is newest - (size-1)",
        bufferOldestSeq() == bufferNewestSeq() - (BUFFER_SIZE - 1));

  // Seq wraps 65535 -> 1 (never 0)
  bufferAck(bufferNewestSeq());
  check("ack newest empties the buffer", bufferCount() == 0 && bufferOldestSeq() == 0);
  while (bufferNewestSeq() != 65535 || bufferCount() == 0) {
    run(1);
    if (bufferCount() > 100) bufferAck(bufferNewestSeq() == 65535 ? 65534 : bufferNewestSeq());
  }
  run(1);
  check("seq wraps 65535 -> 1", bufferNewestSeq() == 1 && bufferGet(65535, 1)->seq == 1);

  // millis() wrap (49.7 days) doesn't break the interval or the clock
  bufferAck(bufferNewestSeq());
  fake_millis = 0xFFFFFFFFUL - 5000;
  clockSet(utc_now);
  bufferUpdate();  // interval since last sample is huge -> one sample now
  uint16_t s1 = bufferNewestSeq();
  run(1);         // crosses the wrap
  check("sample across millis() wrap", bufferNewestSeq() == (uint16_t)(s1 + 1));
  check("UTC across millis() wrap",
        clockAt(bufferGet(s1, 1)->ms) == utc_now + SAMPLE_INTERVAL_MS / 1000);

  printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
