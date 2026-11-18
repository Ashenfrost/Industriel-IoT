/* pm_soc_log.c - workaround for a Zephyr 4.4 link error with CONFIG_PM.
 *
 * The ESP32 PM code (light sleep) logs to a module called "soc" that nothing
 * registers. With CONFIG_LOG and CONFIG_PM both on, the link fails with
 * "undefined reference to log_const_soc". Registering the module here fixes
 * it without turning logging off. Its own file, because a C file can only
 * register one log module.
 */
#include <zephyr/logging/log.h>

#if defined(CONFIG_PM) && defined(CONFIG_LOG)
LOG_MODULE_REGISTER(soc, LOG_LEVEL_WRN);
#endif
