#include "modbusTCP.h"
#include "simPLC.h"
#include "buffer.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ModbusIP_ESP8266.h>

// ---------- Register map (see modbusTCP.h) ----------
// Coil i drives PlcOutput i.
// Each sensor uses two holding registers: temperature at 2*i, humidity at 2*i + 1.
static const uint16_t HREG_HEARTBEAT = 2 * NUM_SENSORS;
static const uint16_t HREG_STATUS    = 2 * NUM_SENSORS + 1;

// Buffer and clock (register map in modbusTCP.h)
static const uint16_t HREG_UTC_HI     = 100;
static const uint16_t HREG_UTC_LO     = 101;
static const uint16_t HREG_CLOCK_SET  = 102;
static const uint16_t HREG_BUF_COUNT  = 103;
static const uint16_t HREG_BUF_OLDEST = 104;
static const uint16_t HREG_BUF_AGE    = 105;
static const uint16_t HREG_BUF_ACK    = 106;
static const uint16_t HREG_BUF_DROP   = 107;
static const uint16_t HREG_WIN_START  = 108;
static const uint16_t HREG_BUF_NEWEST = 109;
static const uint16_t HREG_WIN_BASE   = 200;
static const uint16_t WIN_SAMPLES     = 12;
static const uint16_t WIN_REGS_PER    = 10;
static const uint16_t WIN_REGS        = WIN_SAMPLES * WIN_REGS_PER;

static uint16_t utcHiPending = 0;
static uint16_t windowStart  = 0;

static uint16_t sat16(uint32_t v) { return v > 65535 ? 65535 : (uint16_t)v; }

// ----- Callbacks: run inside mb.task() when the Edge reads/writes -----
// Only registers the Edge writes have set-callbacks: our own mb.Hreg() calls
// would trigger them too.

static uint16_t onSetUtc(TRegister* reg, uint16_t val) {
  if (reg->address.address == HREG_UTC_HI) {
    utcHiPending = val;
  } else {
    uint32_t utc = ((uint32_t)utcHiPending << 16) | val;
    clockSet(utc);
    Serial.printf("Clock set by Edge: %lu\n", (unsigned long)utc);
  }
  return val;
}

static uint16_t onGetUtc(TRegister* reg, uint16_t val) {
  uint32_t now = clockNow();
  return reg->address.address == HREG_UTC_HI ? (uint16_t)(now >> 16) : (uint16_t)now;
}

static uint16_t onSetAck(TRegister* reg, uint16_t val) {
  uint16_t before = bufferCount();
  bufferAck(val);
  Serial.printf("Edge acknowledged up to seq %u: %u sample(s) removed\n", val,
                before - bufferCount());
  return val;
}

static uint16_t onSetWindow(TRegister* reg, uint16_t val) {
  windowStart = val;
  return val;
}

static uint16_t onGetWindow(TRegister* reg, uint16_t val) {
  uint16_t off = reg->address.address - HREG_WIN_BASE;
  const Sample* s = bufferGet(windowStart, off / WIN_REGS_PER);
  if (!s) return 0;  // seq 0 in +0 tells the Edge there's no sample here

  uint32_t utc = clockAt(s->ms);
  uint16_t f = off % WIN_REGS_PER;
  switch (f) {
    case 0: return s->seq;
    case 1: return (uint16_t)(utc >> 16);
    case 2: return (uint16_t)utc;
    case 3: return s->status;
    default: {
      int sensor = (f - 4) / 2;
      return (uint16_t)((f - 4) % 2 == 0 ? s->temp_x10[sensor] : s->hum_x10[sensor]);
    }
  }
}

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

  // Buffer and clock
  mb.addHreg(HREG_UTC_HI, 0, 2);
  mb.onSetHreg(HREG_UTC_HI, onSetUtc, 2);
  mb.onGetHreg(HREG_UTC_HI, onGetUtc, 2);
  mb.addHreg(HREG_CLOCK_SET, 0);
  mb.addHreg(HREG_BUF_COUNT, 0);
  mb.addHreg(HREG_BUF_OLDEST, 0);
  mb.addHreg(HREG_BUF_AGE, 0);
  mb.addHreg(HREG_BUF_ACK, 0);
  mb.onSetHreg(HREG_BUF_ACK, onSetAck);
  mb.addHreg(HREG_BUF_DROP, 0);
  mb.addHreg(HREG_WIN_START, 0);
  mb.onSetHreg(HREG_WIN_START, onSetWindow);
  mb.addHreg(HREG_BUF_NEWEST, 0);
  mb.addHreg(HREG_WIN_BASE, 0, WIN_REGS);
  mb.onGetHreg(HREG_WIN_BASE, onGetWindow, WIN_REGS);

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

  // Buffer status (no callbacks on these, so plain writes)
  mb.Hreg(HREG_CLOCK_SET, clockIsSet() ? 1 : 0);
  mb.Hreg(HREG_BUF_COUNT, bufferCount());
  mb.Hreg(HREG_BUF_OLDEST, bufferOldestSeq());
  mb.Hreg(HREG_BUF_AGE, sat16(bufferOldestAgeS()));
  mb.Hreg(HREG_BUF_DROP, sat16(bufferDropped()));
  mb.Hreg(HREG_BUF_NEWEST, bufferNewestSeq());
}
