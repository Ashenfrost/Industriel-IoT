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
#include <zephyr/sys/timeutil.h>

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
#define T_CNTP_MS     30000

/* Ældre end dette = modemets ur er ikke sat (efter boot uden NITZ står det på 1980). */
#define MODEM_MIN_VALID_YEAR 2024

static const struct modem_cfg *cfg;
static bool ca_uploaded;
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
	ca_uploaded = false; /* ny konfiguration kan have et andet CA */

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

/* ------------------------------------------------------------------ */
/* TLS                                                                 */
/* ------------------------------------------------------------------ */

#define CA_FILE      "ca.pem"
#define T_CFS_MS     10000
#define SSL_CTX      0      /* AT+CSSLCFG-kontekst; AT+SMSSL=1 peger på kontekst 0 */

/* Hostname (til SNI) eller rå IPv4? SNI må ikke indeholde en IP-adresse. */
static bool is_ipv4(const char *s)
{
	for (; *s; s++) {
		if (!((*s >= '0' && *s <= '9') || *s == '.')) {
			return false;
		}
	}
	return true;
}

/* Lægger CA-certifikatet i modemets filsystem og konverterer det til modemets eget
 * format. Én gang pr. boot; filen overlever ikke nødvendigvis et modem-reset. */
static int upload_ca(const char *pem)
{
	size_t len = strlen(pem);
	int ret;

	if (len == 0 || len > 10240) {
		return -EINVAL;
	}
	ret = at_send("AT+CFSINIT", NULL, 0, K_MSEC(T_CONF_MS));
	if (ret) {
		return ret;
	}
	char cmd[64];

	/* dir 3 = /customer, mode 0 = overskriv, timeout i ms til at modtage data */
	snprintk(cmd, sizeof(cmd), "AT+CFSWFILE=3,\"%s\",0,%u,%u", CA_FILE, (unsigned int)len,
		 T_CFS_MS);
	ret = at_send_data(cmd, pem, len, K_MSEC(T_CFS_MS));
	(void)at_send("AT+CFSTERM", NULL, 0, K_MSEC(T_CONF_MS));
	if (ret) {
		LOG_ERR("CA-upload fejlede (%d)", ret);
		return ret;
	}
	/* 2 = rodcertifikat */
	ret = cmdf(T_CONF_MS, NULL, 0, "AT+CSSLCFG=\"CONVERT\",2,\"%s\"", CA_FILE);
	if (ret) {
		LOG_ERR("CA-certifikatet kunne ikke konverteres (%d) - er det gyldig PEM?", ret);
		return ret;
	}
	LOG_INF("CA-certifikat uploadet (%u bytes)", (unsigned int)len);
	return 0;
}

static int tls_configure(void)
{
	bool verify = cfg->ca_cert_pem && cfg->ca_cert_pem[0];
	int ret;

	if (!cfg->tls) {
		return at_send("AT+SMSSL=0", NULL, 0, K_MSEC(T_CONF_MS));
	}
	if (verify && !ca_uploaded) {
		ret = upload_ca(cfg->ca_cert_pem);
		if (ret) {
			return ret;
		}
		ca_uploaded = true;
	}
	/* 3 = TLS 1.2 */
	ret = cmdf(T_CONF_MS, NULL, 0, "AT+CSSLCFG=\"SSLVERSION\",%d,3", SSL_CTX);
	if (ret) {
		return ret;
	}
	if (!is_ipv4(cfg->broker)) {
		ret = cmdf(T_CONF_MS, NULL, 0, "AT+CSSLCFG=\"SNI\",%d,\"%s\"", SSL_CTX, cfg->broker);
		if (ret) {
			return ret;
		}
	}
	if (!verify) {
		LOG_WRN("TLS uden certifikat-tjek (ingen CA sat)");
	}
	return cmdf(T_CONF_MS, NULL, 0, "AT+SMSSL=1,\"%s\",\"\"", verify ? CA_FILE : "");
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
	/* Altid sæt (eller ryd) login, så værdier fra en tidligere konfiguration ikke hænger ved */
	ret = conf_str("USERNAME", cfg->username ? cfg->username : "");
	if (ret) {
		return ret;
	}
	ret = conf_str("PASSWORD", cfg->password ? cfg->password : "");
	if (ret) {
		return ret;
	}
	/* Ingen Will (se modem.h): tomt topic = ingen Will i CONNECT */
	ret = conf_str("TOPIC", "");
	if (ret) {
		return ret;
	}
	ret = conf_str("MESSAGE", "");
	if (ret) {
		return ret;
	}
	ret = tls_configure();
	if (ret) {
		return ret;
	}

	LOG_INF("MQTT connect %s:%u%s", cfg->broker, cfg->port, cfg->tls ? " (TLS)" : "");
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

/* ------------------------------------------------------------------ */
/* UTC-tid                                                             */
/* ------------------------------------------------------------------ */

/* Dato/tid + tidszone i kvarte timer (som SIMCom angiver den) -> ms siden epoch, UTC. */
static int to_epoch_ms(int year, int mon, int day, int hour, int min, int sec, int tz_quarters,
		       int64_t *out)
{
	if (year < MODEM_MIN_VALID_YEAR || mon < 1 || mon > 12 || day < 1 || day > 31 ||
	    hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
		return -EAGAIN;
	}
	struct tm tm = {
		.tm_year = year - 1900,
		.tm_mon = mon - 1,
		.tm_mday = day,
		.tm_hour = hour,
		.tm_min = min,
		.tm_sec = sec,
	};
	/* Modemet angiver lokal tid = UTC + tz, så UTC = lokal tid - tz */
	int64_t t = timeutil_timegm64(&tm) - (int64_t)tz_quarters * 15 * 60;

	*out = t * MSEC_PER_SEC;
	return 0;
}

/* +CNTP: 1,"2026/11/11,09:30:48"  (1 = OK, 61..66 = netværk/DNS/timeout-fejl) */
static int time_from_ntp(const char *server, int64_t *out)
{
	char resp[96];
	int code, y, mo, d, h, mi, s;
	int ret;

	if (bad_str(server)) {
		return -EINVAL;
	}
	ret = at_send("AT+CNTPCID=0", NULL, 0, K_MSEC(T_FAST_MS));
	if (ret) {
		return ret;
	}
	/* tidszone 0, PDP-kontekst 0, mode 2 = sæt modemets ur OG returnér tiden */
	ret = cmdf(T_CONF_MS, NULL, 0, "AT+CNTP=\"%s\",0,0,2", server);
	if (ret) {
		return ret;
	}
	/* Resultatet kommer som en linje efter OK */
	ret = at_send_expect("AT+CNTP", "+CNTP:", resp, sizeof(resp), K_MSEC(T_CNTP_MS));
	if (ret) {
		return ret;
	}
	const char *p = strstr(resp, "+CNTP:");

	if (!p || sscanf(p, "+CNTP: %d", &code) != 1) {
		return -EBADMSG;
	}
	if (code != 1) {
		LOG_WRN("NTP fejlede (kode %d)", code);
		return -EIO;
	}
	if (sscanf(p, "+CNTP: %d,\"%d/%d/%d,%d:%d:%d\"", &code, &y, &mo, &d, &h, &mi, &s) != 7) {
		return -EBADMSG;
	}
	return to_epoch_ms(y, mo, d, h, mi, s, 0, out);
}

/* +CCLK: "26/11/11,10:30:16+04"  (lokal tid, tidszone i kvarte timer) */
static int time_from_cclk(int64_t *out)
{
	char resp[64];
	int y, mo, d, h, mi, s, tz;
	char sign;
	int ret = at_send("AT+CCLK?", resp, sizeof(resp), K_MSEC(T_CONF_MS));

	if (ret) {
		return ret;
	}
	const char *p = strstr(resp, "+CCLK:");

	if (!p || sscanf(p, "+CCLK: \"%d/%d/%d,%d:%d:%d%c%d\"", &y, &mo, &d, &h, &mi, &s, &sign,
			 &tz) != 8) {
		return -EBADMSG;
	}
	if (sign == '-') {
		tz = -tz;
	}
	/* To-cifret år: "80" er modemets standard (1980) = ikke sat */
	y += (y >= 70) ? 1900 : 2000;
	return to_epoch_ms(y, mo, d, h, mi, s, tz, out);
}

int modem_get_utc_ms(const char *ntp_server, int64_t *epoch_ms)
{
	int ret;

	if (!epoch_ms) {
		return -EINVAL;
	}
	if (ntp_server && atomic_get(&pdp_up_flag)) {
		ret = time_from_ntp(ntp_server, epoch_ms);
		if (ret == 0) {
			LOG_INF("UTC fra NTP (%s)", ntp_server);
			return 0;
		}
		LOG_WRN("NTP via modem fejlede (%d), prøver modemets ur", ret);
	}
	ret = time_from_cclk(epoch_ms);
	if (ret == 0) {
		LOG_INF("UTC fra modemets ur (NITZ/RTC)");
	} else {
		LOG_WRN("modemets ur har ingen gyldig tid (%d)", ret);
	}
	return ret;
}
