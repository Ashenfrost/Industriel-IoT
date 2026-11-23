#pragma once

/* SimPLC's IP on the local Wi-Fi. It gets its address by DHCP, so on a new
 * network (e.g. a phone hotspot) override it at build time instead of editing,
 * without quotes (they get lost between PowerShell, CMake and the compiler):
 *   west build ... -- -DEXTRA_CFLAGS=-DMODBUS_SERVER_IP_RAW=192.168.1.50
 * The SimPLC prints its address on its serial port at boot. */
#define MODBUS_STR_(x) #x
#define MODBUS_STR(x)  MODBUS_STR_(x)
#ifdef MODBUS_SERVER_IP_RAW
#define MODBUS_SERVER_IP MODBUS_STR(MODBUS_SERVER_IP_RAW)
#endif
#ifndef MODBUS_SERVER_IP
#define MODBUS_SERVER_IP        "10.57.16.227"  /* SimPLC, check it is on the same subnet */
#endif
#define MODBUS_SERVER_PORT      502
#define MODBUS_UNIT_ID          1
#define MODBUS_POLL_INTERVAL_MS 5000            /* spec: 5-15 s */
#define MODBUS_RX_TIMEOUT_US    1000000         /* microseconds */

/* SimPLC holding registers: 2*i = temp x10, 2*i+1 = humidity x10 */
#define MODBUS_NUM_SENSORS      3               /* index 0/1/2 = top/middle/bottom (IoTSimPLC modbusTCP.h) */
#define MODBUS_REG_START        0
#define MODBUS_REG_COUNT        (2 * MODBUS_NUM_SENSORS + 2)
#define MODBUS_IDX_HEARTBEAT    (2 * MODBUS_NUM_SENSORS)
#define MODBUS_IDX_STATUS       (2 * MODBUS_NUM_SENSORS + 1)  /* bit i set = sensor i error */
/* SimPLC coils 0-5 = top/middle/bottom overheating, top/middle/bottom too humid.
 * The buzzer and display on the PLC follow these coils. */
#define MODBUS_COIL_TEMP_BASE   0
#define MODBUS_COIL_HUM_BASE    3
#define MODBUS_NUM_COILS        (2 * MODBUS_NUM_SENSORS)

/* SimPLC sample buffer and clock (IoTSimPLC include/modbusTCP.h). The SimPLC
 * samples every 10 s by itself; the Edge sets its clock, reads the buffer in
 * windows of MODBUS_WIN_SAMPLES and acknowledges what reached the cloud. */
#define MODBUS_REG_UTC_HI       100  /* R/W, write 100+101 in one request */
#define MODBUS_REG_BUF_STATUS   102  /* 102..109, see MODBUS_BUF_* below */
#define MODBUS_BUF_STATUS_COUNT 8
#define MODBUS_BUF_CLOCK_SET    0    /* index into the 102..109 block */
#define MODBUS_BUF_COUNT        1
#define MODBUS_BUF_OLDEST       2
#define MODBUS_BUF_AGE_S        3
#define MODBUS_BUF_LAST_ACK     4
#define MODBUS_BUF_DROPPED      5
#define MODBUS_BUF_WIN_START    6
#define MODBUS_BUF_NEWEST       7
#define MODBUS_REG_ACK          106  /* W: remove samples up to this seq */
#define MODBUS_REG_WIN_START    108  /* W: first seq of the window */
#define MODBUS_REG_WINDOW       200  /* window: MODBUS_WIN_SAMPLES x 10 regs */
#define MODBUS_WIN_SAMPLES      12
#define MODBUS_WIN_REGS_PER     10   /* seq, utc hi, utc lo, status, 3 x (temp, hum) */
#define MODBUS_SIMPLC_SAMPLE_S  10   /* SimPLC SAMPLE_INTERVAL_MS / 1000 */