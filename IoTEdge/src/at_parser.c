#include "at_parser.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(at_parser, LOG_LEVEL_INF);

struct urc_entry {
	const char *prefix;
	at_urc_cb_t cb;
	void *user;
	uint32_t flags;
};

/* Tilstand for den kommando der venter på svar. Beskyttes af st_mtx. */
struct at_state {
	bool pending;
	const char *cmd;      /* bruges til ekko-skip og til at genkende svarlinjer */
	const char *expect;   /* ekstra påkrævet linje-præfiks (kan være NULL) */
	bool prompt;          /* vent på '>' i stedet for OK */
	bool got_ok;
	bool got_expect;
	int result;
	char *resp;
	size_t resp_len;
	size_t resp_pos;
};

static const struct device *uart_dev;
static bool initialized;

static uint8_t rx_storage[AT_RX_RING_SIZE];
static struct ring_buf rx_rb;
static atomic_t rx_dropped;

static K_SEM_DEFINE(rx_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 1);
static K_MUTEX_DEFINE(cmd_mtx);   /* én kommando ad gangen */
static K_MUTEX_DEFINE(st_mtx);    /* beskytter st */

static K_THREAD_STACK_DEFINE(rx_stack, AT_RX_STACK_SIZE);
static struct k_thread rx_thread;

static struct urc_entry urcs[AT_MAX_URC];
static size_t n_urc;
static struct at_state st;

/* ------------------------------------------------------------------ */
/* Hjælpere (kaldes med st_mtx låst)                                   */
/* ------------------------------------------------------------------ */

static void complete(int result)
{
	st.pending = false;
	st.result = result;
	k_sem_give(&done_sem);
}

static void check_done(void)
{
	if (st.got_ok && (st.expect == NULL || st.got_expect)) {
		complete(0);
	}
}

static void resp_append(const char *line)
{
	if (!st.resp || st.resp_len == 0) {
		return;
	}
	size_t n = strlen(line);

	if (st.resp_pos + n + 2 > st.resp_len) {
		LOG_WRN("resp-buffer fuld, linje droppet");
		return;
	}
	memcpy(&st.resp[st.resp_pos], line, n);
	st.resp_pos += n;
	st.resp[st.resp_pos++] = '\n';
	st.resp[st.resp_pos] = '\0';
}

static bool starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_error(const char *line)
{
	return strcmp(line, "ERROR") == 0 || starts_with(line, "+CME ERROR") ||
	       starts_with(line, "+CMS ERROR");
}

/* "+CEREG: 0,5" er et svar på "AT+CEREG?" - navnet før ':' findes i kommandoen. */
static bool cmd_owns_line(const char *line)
{
	const char *colon = strchr(line, ':');

	if (!st.cmd || line[0] != '+' || !colon) {
		return false;
	}
	char name[24];
	size_t n = (size_t)(colon - line);

	if (n >= sizeof(name)) {
		return false;
	}
	memcpy(name, line, n);
	name[n] = '\0';
	return strstr(st.cmd, name) != NULL;
}

/* ------------------------------------------------------------------ */
/* Linjebehandling (kører i RX-tråden)                                 */
/* ------------------------------------------------------------------ */

static void process_line(const char *line)
{
	at_urc_cb_t cb = NULL;
	void *user = NULL;

	k_mutex_lock(&st_mtx, K_FOREVER);

	if (st.pending) {
		/* 1. ekko af vores egen kommando */
		if (st.cmd && strcmp(line, st.cmd) == 0) {
			goto out;
		}
		/* 2. ventet linje (fx "+APP PDP: 0,ACTIVE") - går forud for URC-dispatch */
		if (st.expect && starts_with(line, st.expect)) {
			resp_append(line);
			st.got_expect = true;
			if (st.got_ok) {
				check_done();
			}
			goto out;
		}
		/* Fil-upload (AT+CFSWFILE) beder om data med linjen "DOWNLOAD" i stedet for ">" */
		if (st.prompt && strcmp(line, "DOWNLOAD") == 0) {
			complete(0);
			goto out;
		}
		/* 3. slutkoder */
		if (strcmp(line, "OK") == 0) {
			st.got_ok = true;
			check_done();
			goto out;
		}
		if (is_error(line)) {
			resp_append(line);
			complete(-EIO);
			goto out;
		}
	}

	/* 4. URC? */
	for (size_t i = 0; i < n_urc; i++) {
		if (!starts_with(line, urcs[i].prefix)) {
			continue;
		}
		if (st.pending && !(urcs[i].flags & AT_URC_ALWAYS) && cmd_owns_line(line)) {
			break; /* svar på den aktuelle kommando, ikke URC */
		}
		cb = urcs[i].cb;
		user = urcs[i].user;
		goto out;
	}

	/* 5. alt andet under en kommando er svar (fx ATI / AT+CGMR har ingen præfiks) */
	if (st.pending) {
		resp_append(line);
	} else {
		LOG_DBG("ukendt linje: %s", line);
	}

out:
	k_mutex_unlock(&st_mtx);
	if (cb) {
		cb(line, user);
	}
}

static char line_buf[AT_LINE_MAX];
static size_t line_len;
static bool line_overflow;

static void feed(uint8_t c)
{
	if (c == '\r' || c == '\n') {
		if (line_overflow) {
			LOG_WRN("linje > %d bytes droppet", AT_LINE_MAX - 1);
		} else if (line_len > 0) {
			line_buf[line_len] = '\0';
			process_line(line_buf);
		}
		line_len = 0;
		line_overflow = false;
		return;
	}
	if (line_len == 0 && c == ' ') {
		return; /* spring ledende mellemrum over (fx "> ") */
	}
	if (line_len >= AT_LINE_MAX - 1) {
		line_overflow = true;
		return;
	}
	line_buf[line_len++] = (char)c;

	/* Prompt ">" kommer uden linjeskift */
	if (line_len == 1 && c == '>') {
		bool consumed = false;

		k_mutex_lock(&st_mtx, K_FOREVER);
		if (st.pending && st.prompt) {
			complete(0);
			consumed = true;
		}
		k_mutex_unlock(&st_mtx);
		if (consumed) {
			line_len = 0;
		}
	}
}

static void rx_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&rx_sem, K_FOREVER);
		uint8_t ch;

		while (ring_buf_get(&rx_rb, &ch, 1) == 1) {
			feed(ch);
		}
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			break;
		}
		uint8_t buf[32];
		int n;

		while ((n = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
			uint32_t put = ring_buf_put(&rx_rb, buf, (uint32_t)n);

			if (put < (uint32_t)n) {
				atomic_add(&rx_dropped, (atomic_val_t)(n - put));
			}
		}
		k_sem_give(&rx_sem);
	}
}

/* ------------------------------------------------------------------ */
/* TX og kommando-flow                                                 */
/* ------------------------------------------------------------------ */

static void tx(const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, p[i]);
	}
}

static void arm(const char *cmd, const char *expect, bool prompt, char *resp, size_t resp_len)
{
	k_mutex_lock(&st_mtx, K_FOREVER);
	memset(&st, 0, sizeof(st));
	st.pending = true;
	st.cmd = cmd;
	st.expect = expect;
	st.prompt = prompt;
	st.resp = resp;
	st.resp_len = resp_len;
	if (resp && resp_len) {
		resp[0] = '\0';
	}
	k_sem_reset(&done_sem);
	k_mutex_unlock(&st_mtx);
}

static int wait_done(k_timeout_t timeout)
{
	int ret;

	if (k_sem_take(&done_sem, timeout) != 0) {
		k_mutex_lock(&st_mtx, K_FOREVER);
		if (st.pending) {
			st.pending = false;
			ret = -ETIMEDOUT;
		} else {
			ret = st.result; /* blev færdig lige ved timeout */
		}
		k_mutex_unlock(&st_mtx);
		return ret;
	}
	k_mutex_lock(&st_mtx, K_FOREVER);
	ret = st.result;
	k_mutex_unlock(&st_mtx);
	return ret;
}

static void send_line(const char *cmd)
{
	tx(cmd, strlen(cmd));
	tx("\r", 1);
}

static int run(const char *cmd, const char *expect, char *resp, size_t resp_len,
	       k_timeout_t timeout)
{
	if (!initialized) {
		return -EBUSY;
	}
	if (!cmd || (resp == NULL) != (resp_len == 0)) {
		return -EINVAL;
	}
	k_mutex_lock(&cmd_mtx, K_FOREVER);
	arm(cmd, expect, false, resp, resp_len);
	LOG_DBG("> %s", cmd);
	send_line(cmd);
	int ret = wait_done(timeout);

	if (ret) {
		LOG_WRN("'%s' -> %d", cmd, ret);
	}
	k_mutex_unlock(&cmd_mtx);
	return ret;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int at_init(const struct device *uart)
{
	if (initialized) {
		return -EALREADY;
	}
	if (!uart || !device_is_ready(uart)) {
		return -ENODEV;
	}
	uart_dev = uart;
	ring_buf_init(&rx_rb, sizeof(rx_storage), rx_storage);

	int ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);

	if (ret < 0) {
		LOG_ERR("UART IRQ API ikke understoettet (%d) - tjek CONFIG_UART_INTERRUPT_DRIVEN",
			ret);
		return ret;
	}
	uart_irq_rx_enable(uart_dev);

	k_thread_create(&rx_thread, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_entry, NULL,
			NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "at_rx");

	initialized = true;
	return 0;
}

int at_register_urc(const char *prefix, at_urc_cb_t cb, void *user, uint32_t flags)
{
	if (!prefix || !cb) {
		return -EINVAL;
	}
	if (n_urc >= AT_MAX_URC) {
		return -ENOSPC;
	}
	k_mutex_lock(&st_mtx, K_FOREVER);
	urcs[n_urc] = (struct urc_entry){ prefix, cb, user, flags };
	n_urc++;
	k_mutex_unlock(&st_mtx);
	return 0;
}

int at_send(const char *cmd, char *resp, size_t resp_len, k_timeout_t timeout)
{
	return run(cmd, NULL, resp, resp_len, timeout);
}

int at_send_expect(const char *cmd, const char *expect, char *resp, size_t resp_len,
		   k_timeout_t timeout)
{
	if (!expect) {
		return -EINVAL;
	}
	return run(cmd, expect, resp, resp_len, timeout);
}

int at_send_data(const char *cmd, const void *data, size_t len, k_timeout_t timeout)
{
	if (!initialized) {
		return -EBUSY;
	}
	if (!cmd || !data || len == 0) {
		return -EINVAL;
	}
	k_mutex_lock(&cmd_mtx, K_FOREVER);

	/* fase 1: kommando -> ">" */
	arm(cmd, NULL, true, NULL, 0);
	send_line(cmd);
	int ret = wait_done(timeout);

	if (ret == 0) {
		/* fase 2: data -> OK. Arm FØR data sendes, så OK ikke overses. */
		arm(cmd, NULL, false, NULL, 0);
		/* Modemet skriver "> " og skal derefter skifte til datatilstand. Sendes data
		 * i samme øjeblik som '>' ses, kan de første bytes gå tabt (set med TLS på
		 * SIM7080: tabte/ødelagte beskeder og "malformed packet" på brokeren). */
		k_msleep(AT_PROMPT_DELAY_MS);
		tx(data, len);
		ret = wait_done(timeout);
	}
	if (ret) {
		LOG_WRN("'%s' (%u bytes) -> %d", cmd, (unsigned int)len, ret);
	}
	k_mutex_unlock(&cmd_mtx);
	return ret;
}

uint32_t at_rx_dropped(void)
{
	return (uint32_t)atomic_get(&rx_dropped);
}
