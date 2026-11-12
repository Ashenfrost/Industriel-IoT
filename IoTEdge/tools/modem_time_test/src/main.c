/* Hardware test for modem_get_utc_ms() + time_sync with the modem as source. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "modem.h"
#include "time_sync.h"

LOG_MODULE_REGISTER(test, LOG_LEVEL_INF);

static const struct modem_cfg cfg = {
	.apn = "onomondo",
	.broker = "unused.invalid", /* MQTT is not used in this test */
	.port = 1883,
	.client_id = "modem-time-test",
};

static int modem_time(int64_t *ms)
{
	return modem_get_utc_ms("pool.ntp.org", ms);
}

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
	int64_t ms;
	int ret;

	k_sleep(K_SECONDS(2));
	LOG_INF("TEST start");

	ret = modem_init(uart, &cfg);
	LOG_INF("TEST modem_init=%d", ret);
	if (ret) {
		return 0;
	}
	ret = modem_attach(120000);
	LOG_INF("TEST modem_attach=%d", ret);

	/* 1: network/RTC clock only, no data connection */
	ret = modem_get_utc_ms(NULL, &ms);
	LOG_INF("TEST CCLK ret=%d utc_ms=%lld", ret, ret ? 0LL : (long long)ms);

	/* 2: NTP via the modem */
	ret = modem_pdp_up(cfg.apn);
	LOG_INF("TEST pdp_up=%d", ret);
	ret = modem_get_utc_ms("pool.ntp.org", &ms);
	LOG_INF("TEST NTP ret=%d utc_ms=%lld", ret, ret ? 0LL : (long long)ms);

	/* 3: time_sync with the modem as source, then read the clock a few times */
	time_sync_set_source(modem_time);
	ret = time_sync_initialize();
	LOG_INF("TEST time_sync_initialize=%d valid=%d", ret, time_sync_is_valid());
	for (int i = 0; i < 3; i++) {
		LOG_INF("TEST now_ms=%llu", (unsigned long long)time_now_ms());
		k_sleep(K_SECONDS(5));
	}

	ret = modem_pdp_down();
	LOG_INF("TEST pdp_down=%d", ret);
	LOG_INF("TEST done");
	return 0;
}
