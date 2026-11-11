/* time_sync.c */
#include "time_sync.h"
#include "app_config.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

LOG_MODULE_REGISTER(time_sync, LOG_LEVEL_INF);

#define SNTP_TIMEOUT_MS 3000
#define SNTP_RETRIES    3

/* epoch_ms = k_uptime_get() + offset_ms. Written on every (re)sync by the
 * MQTT thread, read by the sampling thread. A 64-bit value is not written
 * atomically on a 32-bit CPU, so reads and writes take the spinlock. */
static struct k_spinlock lock;
static int64_t offset_ms;
static atomic_t valid;
static int64_t last_sync_uptime_ms;

/* NULL = SNTP over Zephyr's own network stack (the Wi-Fi path). */
static time_sync_source_t source;

#if defined(CONFIG_SNTP)
static int sntp_epoch_ms(int64_t *epoch_ms)
{
	struct sntp_time ts;
	int ret = -EIO;

	for (int i = 0; i < SNTP_RETRIES; i++) {
		ret = sntp_simple(APP_SNTP_SERVER, SNTP_TIMEOUT_MS, &ts);
		if (ret == 0) {
			break;
		}
		LOG_WRN("SNTP attempt %d failed: %d", i + 1, ret);
	}
	if (ret) {
		LOG_ERR("No SNTP time");
		return ret;
	}
	/* fraction is 1/2^32 s; convert to ms */
	*epoch_ms = (int64_t)ts.seconds * 1000 +
		    (int64_t)(((uint64_t)ts.fraction * 1000U) >> 32);
	return 0;
}
#else
static int sntp_epoch_ms(int64_t *epoch_ms)
{
	ARG_UNUSED(epoch_ms);
	return -ENOTSUP; /* no Zephyr network stack: set a source instead */
}
#endif

void time_sync_set_source(time_sync_source_t src)
{
	source = src;
}

int time_sync_initialize(void)
{
	int64_t epoch_ms;
	int ret = source ? source(&epoch_ms) : sntp_epoch_ms(&epoch_ms);

	if (ret) {
		LOG_ERR("No UTC time (%d), timestamps will be uptime-based", ret);
		return ret;
	}

	K_SPINLOCK(&lock) {
		last_sync_uptime_ms = k_uptime_get();
		offset_ms = epoch_ms - last_sync_uptime_ms;
	}
	atomic_set(&valid, 1);
	LOG_INF("Time synced: %lld s since epoch", (long long)(epoch_ms / 1000));
	return 0;
}

void time_sync_refresh_if_stale(void)
{
	int64_t since_sync;

	K_SPINLOCK(&lock) {
		since_sync = k_uptime_get() - last_sync_uptime_ms;
	}
	if (time_sync_is_valid() &&
	    since_sync < (int64_t)APP_SNTP_RESYNC_S * MSEC_PER_SEC) {
		return;
	}
	(void)time_sync_initialize();
}

bool time_sync_is_valid(void)
{
	return atomic_get(&valid) != 0;
}

uint64_t time_uptime_to_epoch_ms(int64_t uptime_ms)
{
	int64_t epoch_ms = uptime_ms;

	K_SPINLOCK(&lock) {
		if (time_sync_is_valid()) {
			epoch_ms += offset_ms;
		}
	}
	return (uint64_t)epoch_ms;
}

uint64_t time_now_ms(void)
{
	return time_uptime_to_epoch_ms(k_uptime_get());
}
