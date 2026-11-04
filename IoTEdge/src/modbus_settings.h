#pragma once

#define MODBUS_SERVER_IP        "10.57.16.227"  /* SimPLC, check it is on the same subnet */
#define MODBUS_SERVER_PORT      502
#define MODBUS_UNIT_ID          1
#define MODBUS_POLL_INTERVAL_MS 5000            /* spec: 5-15 s */
#define MODBUS_RX_TIMEOUT_US    1000000         /* microseconds */

/* SimPLC holding registers: 2*i = temp x10, 2*i+1 = humidity x10 */
#define MODBUS_NUM_SENSORS      3               /* TODO: confirm; index 0/1/2 = top/mid/bot? */
#define MODBUS_REG_START        0
#define MODBUS_REG_COUNT        (2 * MODBUS_NUM_SENSORS + 2)
#define MODBUS_IDX_HEARTBEAT    (2 * MODBUS_NUM_SENSORS)
#define MODBUS_IDX_STATUS       (2 * MODBUS_NUM_SENSORS + 1)  /* bit i set = sensor i error */
#define MODBUS_COIL_BUZZER      0               /* TODO: confirm */