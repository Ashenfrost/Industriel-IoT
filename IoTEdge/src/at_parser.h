/*
 * at_parser.h - generisk AT-kommandolag oven på en Zephyr UART (interrupt-drevet)
 *
 * Kender intet til SIMCom. Ansvar:
 *  - RX via UART IRQ -> ring buffer -> RX-tråd der samler linjer
 *  - én kommando ad gangen: send, saml svarlinjer, vent på OK / ERROR / +CME ERROR / timeout
 *  - URC-dispatch på prefix (uopfordrede linjer, fx "+APP PDP: 0,DEACTIVE")
 *  - prompt-kommandoer ("AT+SMPUB=..." -> ">" -> rå data -> OK)
 *
 * URC-callbacks kaldes fra RX-tråden: hold dem korte og kald ALDRIG at_send*() herfra.
 */
#ifndef AT_PARSER_H_
#define AT_PARSER_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AT_LINE_MAX
#define AT_LINE_MAX 1152      /* længste linje der kan modtages (inkl. +SMSUB payload) */
#endif
#ifndef AT_RX_RING_SIZE
#define AT_RX_RING_SIZE 2048  /* ISR -> tråd */
#endif
#ifndef AT_RX_STACK_SIZE
#define AT_RX_STACK_SIZE 3072
#endif
#ifndef AT_MAX_URC
#define AT_MAX_URC 16
#endif

/* URC-flag: linjen er ALTID et URC, også selvom den aktuelle kommando har samme præfiks
 * (fx "+SMSUB:" som kan komme midt i svaret på AT+SMSUB=...). */
#define AT_URC_ALWAYS BIT(0)

typedef void (*at_urc_cb_t)(const char *line, void *user);

/** Start RX (IRQ + tråd). Kræver CONFIG_UART_INTERRUPT_DRIVEN. */
int at_init(const struct device *uart);

/** Registrér callback for linjer der starter med prefix (fx "+CEREG:"). prefix skal leve evigt. */
int at_register_urc(const char *prefix, at_urc_cb_t cb, void *user, uint32_t flags);

/**
 * Send cmd (uden \r), vent på OK.
 * Returnerer 0 ved OK, -EIO ved ERROR/+CME ERROR (fejllinjen står i resp),
 * -ETIMEDOUT ved timeout, -EBUSY hvis parseren ikke er klar, -EINVAL ved ugyldige argumenter.
 * resp (valgfri) får alle svarlinjer mellem kommando og OK, adskilt af '\n'.
 */
int at_send(const char *cmd, char *resp, size_t resp_len, k_timeout_t timeout);

/**
 * Som at_send, men lykkes først når OK er set OG en linje der starter med `expect` er set
 * (til kommandoer hvor resultatet kommer som URC efter OK, fx "+APP PDP: 0,ACTIVE").
 */
int at_send_expect(const char *cmd, const char *expect, char *resp, size_t resp_len,
		   k_timeout_t timeout);

/**
 * Prompt-kommando: send cmd, vent på ">" (eller linjen "DOWNLOAD", som AT+CFSWFILE bruger),
 * send `data` (len bytes, binærsikkert), vent på OK.
 * timeout gælder for hver af de to faser.
 */
int at_send_data(const char *cmd, const void *data, size_t len, k_timeout_t timeout);

/** Antal bytes tabt pga. fuld RX-ring siden start (diagnostik). */
uint32_t at_rx_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_PARSER_H_ */
