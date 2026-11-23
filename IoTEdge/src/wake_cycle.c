/* wake_cycle.c - see wake_cycle.h */
#include "wake_cycle.h"
#include "alarm.h"
#include "app_config.h"
#include "modbus_client.h"
#include "modbus_settings.h"
#include "mqtt_client.h"
#include "time_sync.h"
#include "wifi_network.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#include <esp_rom_sys.h>
#include <soc/reset_reasons.h>

LOG_MODULE_REGISTER(wake_cycle, LOG_LEVEL_INF);

/* A whole session (modem attach + 360 samples) takes well under a minute */
#define SEND_TIMEOUT_S     180
/* Never sleep shorter than this, even if a wake-up took very long */
#define MIN_SLEEP_S        10

static uint16_t window[MODBUS_WIN_SAMPLES * MODBUS_WIN_REGS_PER];

static uint16_t next_seq(uint16_t seq)
{
	return seq == 65535 ? 1 : seq + 1; /* the SimPLC skips 0 */
}

/* How many samples `seq` is before `newest` (seq runs 1..65535) */
static uint32_t seq_distance(uint16_t seq, uint16_t newest)
{
	return newest >= seq ? (uint32_t)(newest - seq) : (uint32_t)newest + 65535U - seq;
}

static int read_live(struct modbus_readings *r)
{
	uint16_t regs[MODBUS_REG_COUNT];
	int rc = modbus_client_read_regs(MODBUS_REG_START, regs, MODBUS_REG_COUNT);

	if (rc) {
		return rc;
	}
	for (int i = 0; i < MODBUS_NUM_SENSORS; i++) {
		r->temp_x10[i] = (int16_t)regs[2 * i];
		r->hum_x10[i] = (int16_t)regs[2 * i + 1];
	}
	r->heartbeat = regs[MODBUS_IDX_HEARTBEAT];
	r->status = regs[MODBUS_IDX_STATUS];
	r->timestamp_ms = k_uptime_get();
	return 0;
}

/* Reads every buffered sample into the MQTT queue. *last = seq of the last
 * sample queued (0 = none). Returns the number queued or a negative errno.
 *
 * Timestamps: the SimPLC's UTC of the measurement (PO: "timestamp ved
 * målepunkt"). If its clock isn't set (it rebooted since we last set it)
 * the time is 0; then the sample gets our read time minus 10 s per sample
 * it is older than the newest one ("brug edge"), as uptime that is
 * converted to UTC when sent. */
static int read_buffer(const uint16_t *st, uint16_t *last)
{
	uint16_t start = st[MODBUS_BUF_OLDEST];
	uint16_t newest = st[MODBUS_BUF_NEWEST];
	int64_t read_uptime = k_uptime_get();
	int queued = 0;

	*last = 0;
	if (st[MODBUS_BUF_COUNT] == 0 || start == 0) {
		return 0;
	}
	while (queued < APP_SAMPLE_QUEUE_LEN) {
		int got = 0;
		int rc = modbus_client_write_regs(MODBUS_REG_WIN_START, &start, 1);

		if (rc == 0) {
			rc = modbus_client_read_regs(MODBUS_REG_WINDOW, window, ARRAY_SIZE(window));
		}
		if (rc) {
			return queued ? queued : rc;
		}
		for (int k = 0; k < MODBUS_WIN_SAMPLES; k++) {
			const uint16_t *rec = &window[k * MODBUS_WIN_REGS_PER];

			if (rec[0] == 0) {
				break; /* past the newest sample */
			}
			uint32_t utc = ((uint32_t)rec[1] << 16) | rec[2];
			struct sparkplug_sample s = {
				.plc_status = rec[3],
				.plc_ok = true,
				.utc = utc != 0,
			};

			s.timestamp_ms = utc ? (uint64_t)utc * MSEC_PER_SEC :
				(uint64_t)(read_uptime - (int64_t)seq_distance(rec[0], newest) *
						 MODBUS_SIMPLC_SAMPLE_S * MSEC_PER_SEC);
			for (int i = 0; i < SP_NUM_SENSORS; i++) {
				s.temperature_c[i] = (int16_t)rec[4 + 2 * i] / 10.0f;
				s.humidity_pct[i] = (int16_t)rec[5 + 2 * i] / 10.0f;
			}
			mqtt_client_queue_sample(&s);
			*last = rec[0];
			queued++;
			got++;
		}
		if (got < MODBUS_WIN_SAMPLES) {
			break; /* that was the end of the buffer */
		}
		start = next_seq(*last);
	}
	return queued;
}

/* Writes changed coils. Returns the new mask. */
static uint16_t apply_alarms(const struct modbus_readings *live, uint16_t coils)
{
	uint16_t want = alarm_evaluate(live, coils);
	uint16_t changed = want ^ coils;

	for (int c = 0; c < MODBUS_NUM_COILS; c++) {
		if (changed & (1U << c)) {
			bool on = want & (1U << c);
			int rc = modbus_client_set_coil(c, on);

			LOG_WRN("Alarm coil %d -> %s (%d)", c, on ? "ON" : "off", rc);
		}
	}
	return want;
}

static void set_simplc_clock(void)
{
	if (!time_sync_is_valid()) {
		return;
	}
	uint32_t utc = (uint32_t)(time_now_ms() / MSEC_PER_SEC);
	uint16_t regs[2] = { utc >> 16, utc & 0xFFFF };

	if (modbus_client_write_regs(MODBUS_REG_UTC_HI, regs, 2) == 0) {
		LOG_INF("SimPLC clock set to %u", utc);
	}
}

/* Test mode: APP_DEEP_SLEEP_SIM = 1 waits with k_sleep() instead of a real
 * deep sleep. Same wake logic, but the USB console stays alive, so a test
 * can watch every wake-up (after a real deep sleep the console output is
 * lost). RAM survives here, unlike a real deep sleep. */
#ifndef APP_DEEP_SLEEP_SIM
#define APP_DEEP_SLEEP_SIM 0
#endif

static void deep_sleep(int64_t woke_ms)
{
	int64_t awake_s = (k_uptime_get() - woke_ms) / MSEC_PER_SEC;
	int64_t sleep_s = APP_ALARM_CHECK_S - awake_s;

	if (sleep_s < MIN_SLEEP_S) {
		sleep_s = MIN_SLEEP_S;
	}
#if APP_DEEP_SLEEP_SIM
	LOG_INF("Awake %lld s, simulated deep sleep %lld s", awake_s, sleep_s);
	k_sleep(K_SECONDS(sleep_s));
#else
	LOG_INF("Awake %lld s, deep sleep %lld s", awake_s, sleep_s);
	LOG_PANIC(); /* flush the log before the chip turns off */
	esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * USEC_PER_SEC);
	sys_poweroff();
#endif
}

static void one_wake(bool power_on);

void wake_cycle_run(void)
{
	bool power_on = esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP;

	while (1) { /* a real deep sleep never comes back here */
		one_wake(power_on);
		power_on = false;
	}
}

static void one_wake(bool power_on)
{
	int64_t woke_ms = k_uptime_get();

	LOG_INF("Wake-up (%s): alarm check every %d s, send after %d s",
		power_on ? "power-on/reset" : "deep sleep timer", APP_ALARM_CHECK_S,
		APP_SEND_AGE_S);

	int rc = network_up(APP_WIFI_CONNECT_TIMEOUT_S);
	int64_t wifi_ms = k_uptime_get();
	struct modbus_readings live;
	uint16_t st[MODBUS_BUF_STATUS_COUNT];
	uint16_t coils = 0;

	if (rc == 0) {
		rc = read_live(&live);
	}
	if (rc == 0) {
		rc = modbus_client_read_regs(MODBUS_REG_BUF_STATUS, st, ARRAY_SIZE(st));
	}
	if (rc == 0) {
		rc = modbus_client_read_coils(0, &coils, MODBUS_NUM_COILS);
	}
	LOG_INF("Wi-Fi up in %lld ms, SimPLC read %lld ms (rc %d)", (long long)(wifi_ms - woke_ms),
		(long long)(k_uptime_get() - wifi_ms), rc);
	if (rc) {
		LOG_WRN("SimPLC not reachable (%d): no alarm check, no send", rc);
		goto sleep;
	}

	uint16_t new_coils = apply_alarms(&live, coils);
	bool alarm_changed = new_coils != coils;
	bool due = st[MODBUS_BUF_COUNT] > 0 && st[MODBUS_BUF_AGE_S] >= APP_SEND_AGE_S;

	LOG_INF("SimPLC buffer: %u sample(s), oldest %u s, clock %s, dropped %u",
		st[MODBUS_BUF_COUNT], st[MODBUS_BUF_AGE_S],
		st[MODBUS_BUF_CLOCK_SET] ? "set" : "not set", st[MODBUS_BUF_DROPPED]);

	if (!(due || alarm_changed || power_on)) {
		goto sleep;
	}

	uint16_t last = 0;
	int n = read_buffer(st, &last);

	LOG_INF("Sending %d sample(s) (%s)", n,
		alarm_changed ? "alarm changed" : due ? "buffer due" : "power-on");
	/* Wi-Fi off during the modem session (both radios on at once costs more
	 * than one extra connect of ~5 s). Afterwards Wi-Fi comes up again to set
	 * the SimPLC's clock with the modem's fresh UTC and to acknowledge what
	 * was sent. */
	modbus_client_close();
	network_down();
	rc = mqtt_client_send_now(K_SECONDS(SEND_TIMEOUT_S));
	if (network_up(APP_WIFI_CONNECT_TIMEOUT_S) != 0) {
		LOG_WRN("Wi-Fi not back after the session: no clock/ack this time");
		goto sleep;
	}
	set_simplc_clock();
	if (rc == 0 && last != 0) {
		/* Only now may the SimPLC delete them. If this write is lost, they
		 * are sent again next time (the host stores them once). */
		if (modbus_client_write_regs(MODBUS_REG_ACK, &last, 1) == 0) {
			LOG_INF("Acknowledged up to seq %u", last);
		}
	} else if (rc) {
		LOG_WRN("Send failed (%d): samples stay on the SimPLC", rc);
	}

sleep:
	modbus_client_close();
	network_down();
	deep_sleep(woke_ms);
}
