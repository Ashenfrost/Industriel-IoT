/* time_sync.c */
#include "time_sync.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>

LOG_MODULE_REGISTER(time_sync, LOG_LEVEL_INF);

#define SNTP_TIMEOUT_MS 3000
#define SNTP_RETRIES    3

/* epoch_ms = k_uptime_get() + offset_ms. Written once, read by all threads;
 * a 64-bit store is not atomic on a 32-bit CPU, hence the atomic flag. */
static int64_t offset_ms;
static atomic_t valid;

int time_sync_initialize(void)
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
		LOG_ERR("No SNTP time, timestamps will be uptime-based");
		return ret;
	}

	/* fraction is 1/2^32 s; convert to ms */
	int64_t epoch_ms = (int64_t)ts.seconds * 1000 +
			   (int64_t)(((uint64_t)ts.fraction * 1000U) >> 32);

	offset_ms = epoch_ms - k_uptime_get();
	atomic_set(&valid, 1);
	LOG_INF("Time synced: %lld s since epoch", (long long)ts.seconds);
	return 0;
}

bool time_sync_is_valid(void)
{
	return atomic_get(&valid) != 0;
}

uint64_t time_now_ms(void)
{
	int64_t now = k_uptime_get();

	return (uint64_t)(time_sync_is_valid() ? now + offset_ms : now);
}
