/* Hardware test for the TLS code in modem.c against our own broker.
 * Connect + disconnect only, nothing is published. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "modem.h"
#include "mqtt_settings.h"   /* broker IP + login (from the repo, not printed) */
#include "mqtt_ca_cert.h"    /* our CA */
#include "test_certs.h"      /* TEST_PEM_WRONG */

LOG_MODULE_REGISTER(test, LOG_LEVEL_INF);

#define CFG(id, p, t, ca) { .apn = "onomondo", .broker = MQTT_BROKER_HOSTNAME, .port = (p), \
	.client_id = (id), .username = MQTT_ESP32_USER, .password = MQTT_ESP32_PASS,          \
	.clean_session = true, .tls = (t), .ca_cert_pem = (ca) }

static const struct modem_cfg cfgs[] = {
	CFG("ucl-tls-test-1", 8883, true, MQTT_CA_CERT_PEM),  /* must connect */
	CFG("ucl-tls-test-2", 8883, true, TEST_PEM_WRONG),    /* must fail   */
	CFG("ucl-tls-test-3", 8883, false, NULL),             /* must fail   */
	CFG("ucl-tls-test-4", 1883, false, NULL),             /* must connect (control) */
};
static const char *const names[] = {
	"TLS, our CA", "TLS, wrong CA", "no TLS on 8883", "no TLS on 1883",
};
static const bool expect_ok[] = { true, false, false, true };

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
	int64_t ms;
	int passed = 0;

	k_sleep(K_SECONDS(2));
	for (int i = 0; i < ARRAY_SIZE(cfgs); i++) {
		int ret = modem_init(uart, &cfgs[i]);

		if (ret == 0) {
			ret = modem_attach(120000);
		}
		if (ret == 0) {
			ret = modem_pdp_up(cfgs[i].apn);
		}
		/* TLS checks certificate dates: make sure the modem clock is set */
		if (ret == 0) {
			ret = modem_get_utc_ms("pool.ntp.org", &ms);
		}
		if (ret) {
			LOG_ERR("TEST %d (%s): setup failed %d", i + 1, names[i], ret);
			continue;
		}
		ret = modem_mqtt_connect();
		bool ok = (ret == 0) == expect_ok[i];

		passed += ok;
		LOG_INF("TEST %d (%s): connect=%d -> %s", i + 1, names[i], ret,
			ok ? "PASS" : "FAIL");
		if (ret == 0) {
			modem_mqtt_disconnect();
		}
		modem_pdp_down();
		k_sleep(K_SECONDS(2));
	}
	LOG_INF("TEST done: %d/%d passed", passed, (int)ARRAY_SIZE(cfgs));
	return 0;
}
