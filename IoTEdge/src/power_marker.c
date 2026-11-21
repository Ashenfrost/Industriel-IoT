/* power_marker.c - see power_marker.h */
#include "power_marker.h"

#if APP_POWER_MARKERS

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(power_marker, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

#if !DT_NODE_HAS_PROP(USER_NODE, power_marker_gpios)
#error "APP_POWER_MARKERS needs power-marker-gpios in the zephyr,user node (board overlay)"
#endif

static const struct gpio_dt_spec pins[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, power_marker_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

BUILD_ASSERT(ARRAY_SIZE(pins) >= PM_MARK_COUNT, "need one power-marker-gpios entry per marker");

int power_marker_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		if (!gpio_is_ready_dt(&pins[i])) {
			LOG_ERR("marker GPIO %u not ready", (unsigned int)i);
			return -ENODEV;
		}
		gpio_pin_configure_dt(&pins[i], GPIO_OUTPUT_INACTIVE);
	}
	LOG_INF("power markers on GPIO %d/%d/%d/%d (PPK2 D0-D3)",
		pins[0].pin, pins[1].pin, pins[2].pin, pins[3].pin);
	return 0;
}

void power_marker_set(enum power_marker m, bool on)
{
	if (m < PM_MARK_COUNT) {
		gpio_pin_set_dt(&pins[m], on);
	}
}

#endif /* APP_POWER_MARKERS */
