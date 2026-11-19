/* bdseq_store.c - see bdseq_store.h.
 *
 * Zephyr's settings subsystem stores values under a name ("sp/bdseq").
 * settings_load_subtree("sp") reads everything under "sp/" and calls the
 * handler's set() for each value found. With CONFIG_SETTINGS_NVS the values
 * end up in NVS, which writes new entries one after another and only erases
 * a flash sector when it is full, so the wear is spread out. One write per
 * session (once an hour) is far below what the flash can take.
 */
#include "bdseq_store.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(bdseq_store, LOG_LEVEL_INF);

#define SUBTREE "sp"
#define KEY     "bdseq"

static uint8_t loaded_value;
static bool loaded;

/* Called by settings_load_subtree() for every value under "sp/" */
static int sp_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (!settings_name_steq(name, KEY, &next) || next) {
		return -ENOENT;
	}
	if (len != sizeof(loaded_value)) {
		return -EINVAL;
	}
	ssize_t n = read_cb(cb_arg, &loaded_value, sizeof(loaded_value));

	if (n < 0) {
		return (int)n;
	}
	loaded = (n == sizeof(loaded_value));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sp, SUBTREE, NULL, sp_set, NULL, NULL);

int bdseq_store_load(uint8_t *out)
{
	int ret = settings_subsys_init();

	if (ret) {
		return ret;
	}
	loaded = false;
	ret = settings_load_subtree(SUBTREE);
	if (ret) {
		return ret;
	}
	if (!loaded) {
		return -ENOENT;
	}
	*out = loaded_value;
	return 0;
}

int bdseq_store_save(uint8_t bd_seq)
{
	return settings_save_one(SUBTREE "/" KEY, &bd_seq, sizeof(bd_seq));
}
