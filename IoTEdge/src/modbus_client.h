#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "modbus_settings.h"

struct modbus_readings {
	int16_t temp_x10[MODBUS_NUM_SENSORS];
	int16_t hum_x10[MODBUS_NUM_SENSORS];
	uint16_t heartbeat;
	uint16_t status;        /* bit i set = sensor i failed its last read */
	int64_t timestamp_ms;   /* k_uptime_get() when received */
};

int modbus_client_initialize(void);
int modbus_client_get_readings(struct modbus_readings *out);  /* -ENODATA if none yet */
/* One read right now (Wi-Fi must be up), socket closed afterwards. Used in
 * APP_WIFI_ON_DEMAND mode instead of the background poll thread. */
int modbus_client_read_now(struct modbus_readings *out);
int modbus_client_set_coil(uint16_t coil, bool on);

/* Generic transactions for the deep sleep wake cycle (wake_cycle.c). Wi-Fi
 * must be up. The TCP socket stays open between calls; on an error it is
 * closed and the call retried once. modbus_client_close() closes it before
 * Wi-Fi goes down. */
int modbus_client_read_regs(uint16_t start, uint16_t *regs, uint16_t count);
int modbus_client_write_regs(uint16_t start, const uint16_t *regs, uint16_t count);
/* Coils as a bitmask: bit i = coil start + i (count <= 16) */
int modbus_client_read_coils(uint16_t start, uint16_t *mask, uint16_t count);
void modbus_client_close(void);