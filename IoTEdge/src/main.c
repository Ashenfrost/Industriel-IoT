#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_config.h"
#include "client_id.h"
#include "wifi_network.h"
#include "mqtt_client.h"
#include "time_sync.h"
#include "modbus_client.h"
#include "power_marker.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* A Modbus reading older than this is treated as missing (2 polls + slack) */
#define PLC_MAX_AGE_MS (2 * MODBUS_POLL_INTERVAL_MS + 1000)

BUILD_ASSERT(MODBUS_NUM_SENSORS == SP_NUM_SENSORS,
	     "Modbus register map and Sparkplug metrics must have the same sensors");

#ifdef APP_FAKE_PLC_DATA
/* Test mode: made-up PLC values, so the whole chain (Sparkplug -> broker ->
 * QuestDB) can be tested without the SimPLC. Every 6th sample flags the
 * bottom sensor as failed, to exercise the null values. */
static int fake_plc_sample(struct sparkplug_sample *s)
{
	static uint32_t n;

	n++;
	for (int i = 0; i < SP_NUM_SENSORS; i++) {
		s->temperature_c[i] = 20.0f + 2.0f * i + (float)(n % 5);
		s->humidity_pct[i] = 40.0f + 5.0f * i + (float)(n % 3);
	}
	s->plc_status = (n % 6 == 0) ? 0x4 : 0;
	s->plc_ok = true;
	return 0;
}
#endif

/* Fills s from the latest SimPLC reading. Sets s->plc_ok = false (all values
 * become null in Sparkplug) when there is no fresh reading or the PLC's
 * heartbeat has stopped, i.e. its scan cycle hangs and the values are old. */
static void plc_sample(struct sparkplug_sample *s)
{
#ifdef APP_FAKE_PLC_DATA
	fake_plc_sample(s);
#else
	static uint16_t last_heartbeat;
	static bool have_last;
	struct modbus_readings r;

	s->plc_ok = false;
	if (modbus_client_get_readings(&r) != 0) {
		LOG_WRN("no Modbus data from SimPLC");
		return;
	}
	if (k_uptime_get() - r.timestamp_ms > PLC_MAX_AGE_MS) {
		LOG_WRN("Modbus data is %lld ms old", (long long)(k_uptime_get() - r.timestamp_ms));
		return;
	}
	if (r.heartbeat == 0 || (have_last && r.heartbeat == last_heartbeat)) {
		LOG_WRN("SimPLC heartbeat not running (%u)", r.heartbeat);
		last_heartbeat = r.heartbeat;
		have_last = true;
		return;
	}
	last_heartbeat = r.heartbeat;
	have_last = true;

	for (int i = 0; i < SP_NUM_SENSORS; i++) {
		s->temperature_c[i] = r.temp_x10[i] / 10.0f;
		s->humidity_pct[i] = r.hum_x10[i] / 10.0f;
	}
	s->plc_status = r.status;
	s->plc_ok = true;
	/* Stamp with the time the PLC values were read, not when we sampled */
	s->timestamp_ms = (uint64_t)r.timestamp_ms;

	LOG_INF("PLC x10 temp %d %d %d, hum %d %d %d, status 0x%x",
		r.temp_x10[0], r.temp_x10[1], r.temp_x10[2],
		r.hum_x10[0], r.hum_x10[1], r.hum_x10[2], r.status);
#endif
}

int main(void)
{
	LOG_INF("Zephyr app started");
	power_marker_init();

	build_client_id_initialize();
	LOG_INF("client id: %s", build_client_id_get());

	/* Wi-Fi: the local network to the SimPLC (Modbus TCP), and the uplink
	 * when APP_UPLINK_CELLULAR = 0. APP_DISABLE_WIFI (test only, with
	 * APP_FAKE_PLC_DATA) keeps the Wi-Fi radio off. */
#ifdef APP_DISABLE_WIFI
	LOG_WRN("APP_DISABLE_WIFI: Wi-Fi radio not started");
#else
	if (network_initialize() != 0) {
		LOG_ERR("network init failed");
	}
#endif

#if !APP_UPLINK_CELLULAR
	/* Real UTC timestamps for Sparkplug. If this fails, the MQTT thread
	 * retries before each session. With the cellular uplink the MQTT thread
	 * gets the time from the modem once the data connection is up. */
	time_sync_initialize();
#endif

	mqtt_client_initialize();

#ifdef APP_FAKE_PLC_DATA
	LOG_WRN("APP_FAKE_PLC_DATA: sending made-up PLC values, not the SimPLC's");
#else
	if (modbus_client_initialize() != 0) {
		LOG_ERR("modbus init failed");
	}
#endif

	/* Application thread: sample on a fixed interval and queue the result.
	 * The MQTT thread batches and publishes, so a slow or broken network
	 * never shifts the sampling interval. */
	int64_t next = k_uptime_get();

	while (1) {
		/* Stamped with uptime (replaced by the PLC read time when there is
		 * fresh data); converted to UTC when it is sent */
		struct sparkplug_sample s = { .timestamp_ms = (uint64_t)k_uptime_get() };

		power_marker_set(PM_MARK_SAMPLE, true);
		plc_sample(&s);
		/* Queued even without PLC data: the nulls show the gap in QuestDB */
		mqtt_client_queue_sample(&s);
		power_marker_set(PM_MARK_SAMPLE, false);

		next += APP_SAMPLE_INTERVAL_S * MSEC_PER_SEC;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}

	return 0;
}
