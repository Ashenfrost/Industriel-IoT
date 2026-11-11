/*
 * modem.h - SIM7080G (M5STAMP:CatM) over AT: opstart, netværk, PDP og MQTT via modulets
 * indbyggede MQTT-klient (AT+SMCONF / SMCONN / SMPUB / SMSUB).
 *
 * Alle kald er blokerende og skal kaldes fra én tråd (fx main). modem_msg_cb_t kaldes dog
 * fra AT-parserens RX-tråd: hold den kort, kopiér data og kald ikke modem_*() derfra.
 */
#ifndef MODEM_H_
#define MODEM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_TOPIC_MAX        96
#define MODEM_MAX_SUBS         4
#define MODEM_MQTT_MAX_PAYLOAD 1024   /* grænse for AT+SMPUB på SIM7080 */

struct modem_cfg {
	const char *apn;          /* "onomondo" */
	const char *broker;       /* IP eller hostname på IoTPlatform-brokeren */
	uint16_t port;            /* 1883 */
	const char *client_id;
	const char *username;     /* NULL eller "" = ingen */
	const char *password;
	uint16_t keepalive_s;     /* 0 = 60 */
	bool clean_session;
};

typedef void (*modem_msg_cb_t)(const char *topic, const uint8_t *payload, size_t len, void *user);

/** Start AT-laget, vent på modulet, ATE0, AT+CMEE=2, tjek SIM. cfg skal leve evigt (static). */
int modem_init(const struct device *uart, const struct modem_cfg *cfg);

/** Vent på LTE-registrering (CEREG 1 eller 5). */
int modem_attach(uint32_t timeout_ms);

/** Aktivér PDP-kontekst 0 med APN (springes over hvis der allerede er en IP). */
int modem_pdp_up(const char *apn);
int modem_pdp_down(void);

/** Forbind til brokeren og gen-abonnér. Springer over hvis forbindelsen allerede er oppe. */
int modem_mqtt_connect(void);
int modem_mqtt_disconnect(void);
bool modem_mqtt_is_connected(void);

/** attach + pdp_up + mqtt_connect. Idempotent: kald gentagne gange fra main ved fejl. */
int modem_connect(void);

/** qos 0..2. Payload er binærsikkert (fx Sparkplug B), max MODEM_MQTT_MAX_PAYLOAD. */
int modem_mqtt_publish(const char *topic, const void *payload, size_t len, int qos, bool retain);

/** Husker abonnementet og gentegner det automatisk efter reconnect. */
int modem_mqtt_subscribe(const char *topic, int qos);

void modem_set_message_cb(modem_msg_cb_t cb, void *user);

/**
 * UTC-tid fra modemet i ms siden Unix epoch (opløsning 1 s).
 *
 * Med ntp_server (kræver PDP oppe, se modem_pdp_up): AT+CNTP mod den server. Modemets
 * ur sættes samtidig til UTC. Fejler NTP, eller er ntp_server NULL, bruges modemets ur
 * via AT+CCLK? - det er tiden fra mobilnettet (NITZ) eller den sidste NTP-sync.
 *
 * Returnerer 0, -EAGAIN hvis modemet ikke har en gyldig tid endnu, ellers AT-fejl.
 * Skal kaldes fra samme tråd som de andre modem_*()-kald.
 */
int modem_get_utc_ms(const char *ntp_server, int64_t *epoch_ms);

#ifdef __cplusplus
}
#endif

#endif /* MODEM_H_ */
