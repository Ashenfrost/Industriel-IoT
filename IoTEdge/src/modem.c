#include "modem.h"
#include "at_parser.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

#define CMD_MAX 256

/* Timeouts (ms) - justér efter erfaring */
#define T_FAST_MS     1000
#define T_CONF_MS     5000
#define T_PDP_MS      20000
#define T_SMCONN_MS   60000
#define T_SMSUB_MS    10000
#define T_SMPUB_QOS0  5000
#define T_SMPUB_QOS12 15000

static const struct modem_cfg *cfg;
static bool urcs_registered;
static atomic_t mqtt_up;
static atomic_t pdp_up_flag;

static modem_msg_cb_t msg_cb;
static void *msg_user;

static struct {
	char topic[MODEM_TOPIC_MAX];
	uint8_t qos;
	bool used;
} subs[MODEM_MAX_SUBS];

/* ------------------------------------------------------------------ */
/* Hjælpere                                                            */
/* ------------------------------------------------------------------ */

static bool bad_str(const char *s)
{
	return !s || strchr(s, '"') || strchr(s, '\r') || strchr(s, '\n');
}

static int cmdf(uint32_t timeout_ms, char *resp, size_t resp_len, const char *fmt, ...)
{
	char cmd[CMD_MAX];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintk(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(cmd)) {
		return -ENOMEM;
	}
	return at_send(cmd, resp, resp_len, K_MSEC(timeout_ms));
}

static int conf_str(const char *key, const char *val)
{
	if (bad_str(val)) {
		return -EINVAL;
	}
	return cmdf(T_CONF_MS, NULL, 0, "AT+SMCONF=\"%s\",\"%s\"", key, val);
}

static int conf_num(const char *key, unsigned int val)
{
	return cmdf(T_CONF_MS, NULL, 0, "AT+SMCONF=\"%s\",%u", key, val);
}

/* ------------------------------------------------------------------ */
/* URC-håndtering                                                      */
/* ------------------------------------------------------------------ */

static void urc_ignore(const char *line, void *user)
{
	ARG_UNUSED(user);
	LOG_DBG("URC: %s", line);
}

static void urc_pdp(const char *line, void *user)
{
	ARG_UNUSED(user);
	LOG_INF("%s", line);
	if (strstr(line, "DEACTIVE")) {
		atomic_set(&pdp_up_flag, 0);
		atomic_set(&mqtt_up, 0);
	}
}

static void urc_smstate(const char *line, void *user)
{
	int s;

	ARG_UNUSED(user);
	if (sscanf(line, "+SMSTATE: %d", &s) == 1) {
		LOG_INF("MQTT state %d", s);
		atomic_set(&mqtt_up, s > 0);
	}
}

/* +SMSUB: "topic","payload" */
static void urc_smsub(const char *line, void *user)
{
	ARG_UNUSED(user);

	const char *p = strchr(line, '"');

	if (!p) {
		return;
	}
	p++;
	const char *q = strstr(p, "\",\"");

	if (!q) {
		return;
	}
	char topic[MODEM_TOPIC_MAX];
	size_t tl = (size_t)(q - p);

	if (tl >= sizeof(topic)) {
		LOG_WRN("topic for lang");
		return;
	}
	memcpy(topic, p, tl);
	topic[tl] = '\0';

	const char *pl = q + 3;
	size_t plen = strlen(pl);

	if (plen && pl[plen - 1] == '"') {
		plen--;
	}
	if (msg_cb) {
		msg_cb(topic, (const uint8_t *)pl, plen, msg_user);
	}
}

static void register_urcs(void)
{
	if (urcs_registered) {
		return;
	}
	at_register_urc("+SMSUB:", urc_smsub, NULL, AT_URC_ALWAYS);
	at_register_urc("+SMSTATE:", urc_smstate, NULL, 0);
	at_register_urc("+APP PDP:", urc_pdp, NULL, 0);
	/* Støj ved boot / netværksskift - sluges så den ikke havner i svar-buffere */
	at_register_urc("+CPIN:", urc_ignore, NULL, 0);
	at_register_urc("+CEREG:", urc_ignore, NULL, 0);
	at_register_urc("*PSUTTZ:", urc_ignore, NULL, 0);
	at_register_urc("DST:", urc_ignore, NULL, 0);
	at_register_urc("SMS Ready", urc_ignore, NULL, 0);
	at_register_urc("Call Ready", urc_ignore, NULL, 0);
	urcs_registered = true;
}

/* ------------------------------------------------------------------ */
/* Opstart og netværk                                                  */
/* ------------------------------------------------------------------ */

int modem_init(const struct device *uart, const struct modem_cfg *c)
{
	int ret;

	if (!c || !c->broker || !c->client_id) {
		return -EINVAL;
	}
	cfg = c;

	ret = at_init(uart);
	if (ret && ret != -EALREADY) {
		return ret;
	}
	register_urcs();

	/* Modulet kan stadig være ved at boote */
	ret = -ETIMEDOUT;
	for (int i = 0; i < 15 && ret; i++) {
		ret = at_send("AT", NULL, 0, K_MSEC(T_FAST_MS));
	}
	if (ret) {
		LOG_ERR("modulet svarer ikke paa AT");
		return -ETIMEDOUT;
	}
	ret = at_send("ATE0", NULL, 0, K_MSEC(T_FAST_MS));
	if (ret) {
		return ret;
	}
	at_send("AT+CMEE=2", NULL, 0, K_MSEC(T_FAST_MS)); /* tekstfejl i stedet for tal */

	char resp[48];

	ret = -ENODEV;
	for (int i = 0; i < 10 && ret; i++) {
		if (at_send("AT+CPIN?", resp, sizeof(resp), K_MSEC(T_CONF_MS)) == 0 &&
		    strstr(resp, "READY")) {
			ret = 0;
		} else {
			k_sleep(K_SECONDS(1));
		}
	}
	if (ret) {
		LOG_ERR("SIM ikke klar");
		return ret;
	}
	LOG_INF("modem klar");
	return 0;
}

int modem_attach(uint32_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;
	char resp[48];

	do {
		if (at_send("AT+CEREG?", resp, sizeof(resp), K_MSEC(T_CONF_MS)) == 0) {
			int n, stat;
			const char *p = strstr(resp, "+CEREG:");

			if (p && sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2 &&
			    (stat == 1 || stat == 5)) {
				LOG_INF("registreret (CEREG %d)", stat);
				return 0;
			}
		}
		k_sleep(K_SECONDS(2));
	} while (k_uptime_get() < end);

	LOG_ERR("ingen LTE-registrering");
	return -ETIMEDOUT;
}

static bool pdp_has_ip(void)
{
	char resp[192];

	if (at_send("AT+CNACT?", resp, sizeof(resp), K_MSEC(T_CONF_MS)) != 0) {
		return false;
	}
	const char *p = strstr(resp, "+CNACT: 0,");
	int state;
	char ip[20];

	if (p && sscanf(p, "+CNACT: 0,%d,\"%19[^\"]\"", &state, ip) == 2 && state == 1) {
		LOG_INF("PDP aktiv, IP %s", ip);
		return true;
	}
	return false;
}

int modem_pdp_up(const char *apn)
{
	int ret;

	if (bad_str(apn)) {
		return -EINVAL;
	}
	if (pdp_has_ip()) {
		atomic_set(&pdp_up_flag, 1);
		return 0;
	}
	ret = cmdf(T_CONF_MS, NULL, 0, "AT+CNCFG=0,1,\"%s\"", apn);
	if (ret) {
		return ret;
	}
	ret = at_send_expect("AT+CNACT=0,1", "+APP PDP: 0,ACTIVE", NULL, 0, K_MSEC(T_PDP_MS));
	if (ret == 0) {
		atomic_set(&pdp_up_flag, 1);
		pdp_has_ip(); /* logger IP */
	}
	return ret;
}

int modem_pdp_down(void)
{
	atomic_set(&mqtt_up, 0);
	atomic_set(&pdp_up_flag, 0);
	return at_send("AT+CNACT=0,0", NULL, 0, K_MSEC(T_PDP_MS));
}

/* ------------------------------------------------------------------ */
/* MQTT                                                                */
/* ------------------------------------------------------------------ */

static int smstate_query(void)
{
	char resp[48];
	int s = -1;

	if (at_send("AT+SMSTATE?", resp, sizeof(resp), K_MSEC(T_CONF_MS)) != 0) {
		return -EIO;
	}
	const char *p = strstr(resp, "+SMSTATE:");

	if (!p || sscanf(p, "+SMSTATE: %d", &s) != 1) {
		return -EBADMSG;
	}
	atomic_set(&mqtt_up, s > 0);
	return s;
}

static int sub_at(const char *topic, int qos)
{
	return cmdf(T_SMSUB_MS, NULL, 0, "AT+SMSUB=\"%s\",%d", topic, qos);
}

int modem_mqtt_connect(void)
{
	int ret;

	if (!cfg) {
		return -EINVAL;
	}
	if (smstate_query() > 0) {
		return 0; /* allerede forbundet */
	}
	if (bad_str(cfg->broker) || bad_str(cfg->client_id)) {
		return -EINVAL;
	}

	ret = cmdf(T_CONF_MS, NULL, 0, "AT+SMCONF=\"URL\",\"%s\",%u", cfg->broker, cfg->port);
	if (ret) {
		return ret;
	}
	ret = conf_num("KEEPTIME", cfg->keepalive_s ? cfg->keepalive_s : 60);
	if (ret) {
		return ret;
	}
	ret = conf_num("CLEANSS", cfg->clean_session ? 1 : 0);
	if (ret) {
		return ret;
	}
	ret = conf_str("CLIENTID", cfg->client_id);
	if (ret) {
		return ret;
	}
	if (cfg->username && cfg->username[0]) {
		ret = conf_str("USERNAME", cfg->username);
		if (ret) {
			return ret;
		}
		ret = conf_str("PASSWORD", cfg->password ? cfg->password : "");
		if (ret) {
			return ret;
		}
	}

	LOG_INF("MQTT connect %s:%u", cfg->broker, cfg->port);
	ret = at_send("AT+SMCONN", NULL, 0, K_MSEC(T_SMCONN_MS));
	if (ret) {
		return ret;
	}
	atomic_set(&mqtt_up, 1);

	/* Gen-abonnér (relevant ved clean session / reconnect) */
	for (int i = 0; i < MODEM_MAX_SUBS; i++) {
		if (subs[i].used) {
			int r = sub_at(subs[i].topic, subs[i].qos);

			if (r) {
				LOG_WRN("resubscribe '%s' fejlede (%d)", subs[i].topic, r);
			}
		}
	}
	return 0;
}

int modem_mqtt_disconnect(void)
{
	atomic_set(&mqtt_up, 0);
	return at_send("AT+SMDISC", NULL, 0, K_MSEC(T_CONF_MS));
}

bool modem_mqtt_is_connected(void)
{
	return atomic_get(&mqtt_up) != 0;
}

int modem_connect(void)
{
	int ret = modem_attach(120000);

	if (ret) {
		return ret;
	}
	ret = modem_pdp_up(cfg->apn);
	if (ret) {
		return ret;
	}
	return modem_mqtt_connect();
}

int modem_mqtt_publish(const char *topic, const void *payload, size_t len, int qos, bool retain)
{
	if (bad_str(topic) || !payload || qos < 0 || qos > 2) {
		return -EINVAL;
	}
	if (len == 0 || len > MODEM_MQTT_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}
	if (!modem_mqtt_is_connected()) {
		return -ENOTCONN;
	}

	char cmd[CMD_MAX];
	int n = snprintk(cmd, sizeof(cmd), "AT+SMPUB=\"%s\",%u,%d,%d", topic, (unsigned int)len,
			 qos, retain ? 1 : 0);

	if (n < 0 || n >= (int)sizeof(cmd)) {
		return -ENOMEM;
	}
	int ret = at_send_data(cmd, payload, len,
			       K_MSEC(qos ? T_SMPUB_QOS12 : T_SMPUB_QOS0));

	if (ret) {
		smstate_query(); /* opdatér cached forbindelsesstatus */
	}
	return ret;
}

int modem_mqtt_subscribe(const char *topic, int qos)
{
	if (bad_str(topic) || strlen(topic) >= MODEM_TOPIC_MAX || qos < 0 || qos > 2) {
		return -EINVAL;
	}
	int slot = -1;

	for (int i = 0; i < MODEM_MAX_SUBS; i++) {
		if (subs[i].used && strcmp(subs[i].topic, topic) == 0) {
			slot = i;
			break;
		}
		if (!subs[i].used && slot < 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		return -ENOSPC;
	}
	strncpy(subs[slot].topic, topic, MODEM_TOPIC_MAX - 1);
	subs[slot].topic[MODEM_TOPIC_MAX - 1] = '\0';
	subs[slot].qos = (uint8_t)qos;
	subs[slot].used = true;

	if (!modem_mqtt_is_connected()) {
		return 0; /* tegnes ved næste connect */
	}
	return sub_at(topic, qos);
}

void modem_set_message_cb(modem_msg_cb_t cb, void *user)
{
	msg_cb = cb;
	msg_user = user;
}
