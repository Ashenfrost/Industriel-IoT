/* Modbus TCP master: Zephyr raw-ADU client + our own TCP transport */
#include "modbus_client.h"
#include "modbus_settings.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(modbus_edge, LOG_LEVEL_INF);

#define ADU_DATA_MAX     (CONFIG_MODBUS_BUFFER_SIZE - 4)
#define POLL_STACK_SIZE  4096
#define IO_TIMEOUT_MS    1000

static int iface = -1;
static int sock = -1;

static K_MUTEX_DEFINE(txn_lock);    /* one transaction at a time, owns sock */
static K_MUTEX_DEFINE(data_lock);   /* protects latest */
static struct modbus_readings latest;
static bool latest_valid;

static K_THREAD_STACK_DEFINE(poll_stack, POLL_STACK_SIZE);
static struct k_thread poll_thread_data;

static void sock_close(void)
{
	if (sock >= 0) {
		zsock_close(sock);
		sock = -1;
	}
}

static int sock_open(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(MODBUS_SERVER_PORT),
	};
	int fd;

	if (zsock_inet_pton(AF_INET, MODBUS_SERVER_IP, &addr.sin_addr) != 1) {
		LOG_ERR("Invalid Modbus server IP");
		return -EINVAL;
	}

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}

	if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = -errno;

		zsock_close(fd);
		LOG_WRN("Modbus TCP connect failed: %d", err);
		return err;
	}

	sock = fd;
	LOG_INF("Modbus TCP connected");
	return 0;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = zsock_send(fd, buf + sent, len - sent, 0);

		if (n < 0) {
			return -errno;
		}
		sent += n;
	}
	return 0;
}

static int recv_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t got = 0;

	while (got < len) {
		int64_t left = deadline - k_uptime_get();
		struct zsock_pollfd p = { .fd = fd, .events = ZSOCK_POLLIN };
		ssize_t n;
		int rc;

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		rc = zsock_poll(&p, 1, (int)left);
		if (rc < 0) {
			return -errno;
		}
		if (rc == 0) {
			return -ETIMEDOUT;
		}
		n = zsock_recv(fd, buf + got, len - got, 0);
		if (n < 0) {
			return -errno;
		}
		if (n == 0) {
			return -ECONNRESET;
		}
		got += n;
	}
	return 0;
}

/* Called by Zephyr's client in the caller's thread with the request ADU.
 * We send it, read and validate the reply, and hand it back with
 * modbus_raw_submit_rx(). The return value is ignored by Zephyr; if we
 * submit nothing, the client times out.
 */
static int raw_tx_cb(const int idx, const struct modbus_adu *adu, void *user_data)
{
	uint8_t frame[MODBUS_MBAP_AND_FC_LENGTH + ADU_DATA_MAX];
	uint8_t hdr[MODBUS_MBAP_AND_FC_LENGTH];
	struct modbus_adu rx = {0};
	uint16_t mbap_len;
	int rc;

	ARG_UNUSED(user_data);

	if (sock < 0) {
		return -ENOTCONN;
	}
	if (adu->length > ADU_DATA_MAX) {
		return -EMSGSIZE;
	}

	modbus_raw_put_header(adu, frame);
	memcpy(&frame[MODBUS_MBAP_AND_FC_LENGTH], adu->data, adu->length);

	rc = send_all(sock, frame, MODBUS_MBAP_AND_FC_LENGTH + adu->length);
	if (rc) {
		goto fail;
	}

	rc = recv_exact(sock, hdr, sizeof(hdr), IO_TIMEOUT_MS);
	if (rc) {
		goto fail;
	}

	/* Input validation (62443 CR 3.5): length, protocol, ids, function code */
	mbap_len = sys_get_be16(&hdr[4]);
	if (mbap_len < 2 || (mbap_len - 2) > ADU_DATA_MAX) {
		rc = -EBADMSG;
		goto fail;
	}

	modbus_raw_get_header(&rx, hdr);
	if (rx.proto_id != 0 || rx.trans_id != adu->trans_id ||
	    rx.unit_id != adu->unit_id ||
	    (rx.fc != adu->fc && rx.fc != (adu->fc | 0x80))) {
		rc = -EPROTO;
		goto fail;
	}

	rc = recv_exact(sock, rx.data, rx.length, IO_TIMEOUT_MS);
	if (rc) {
		goto fail;
	}

	return modbus_raw_submit_rx(idx, &rx);

fail:
	LOG_WRN("Modbus TCP transaction failed: %d", rc);
	sock_close();   /* any error: drop the connection, poll thread reconnects */
	return rc;
}

static int connect_locked(void)
{
	return (sock >= 0) ? 0 : sock_open();
}

static void store_readings(const uint16_t *regs)
{
	k_mutex_lock(&data_lock, K_FOREVER);
	for (int i = 0; i < MODBUS_NUM_SENSORS; i++) {
		latest.temp_x10[i] = (int16_t)regs[2 * i];
		latest.hum_x10[i] = (int16_t)regs[2 * i + 1];
	}
	latest.heartbeat = regs[MODBUS_IDX_HEARTBEAT];
	latest.status = regs[MODBUS_IDX_STATUS];
	latest.timestamp_ms = k_uptime_get();
	latest_valid = true;
	k_mutex_unlock(&data_lock);
}

static void poll_fn(void *p1, void *p2, void *p3)
{
	uint16_t regs[MODBUS_REG_COUNT];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		int64_t t0 = k_uptime_get();
		int rc;

		k_mutex_lock(&txn_lock, K_FOREVER);
		rc = connect_locked();
		if (rc == 0) {
			rc = modbus_read_holding_regs(iface, MODBUS_UNIT_ID, MODBUS_REG_START,
						      regs, MODBUS_REG_COUNT);
		}
		k_mutex_unlock(&txn_lock);

		if (rc == 0) {
			store_readings(regs);
		} else {
			LOG_WRN("Modbus poll failed: %d", rc);
			k_mutex_lock(&data_lock, K_FOREVER);
			latest_valid = false;
			k_mutex_unlock(&data_lock);
		}

		int64_t wait = MODBUS_POLL_INTERVAL_MS - (k_uptime_get() - t0);

		k_sleep(K_MSEC(wait > 0 ? wait : 0));
	}
}

int modbus_client_get_readings(struct modbus_readings *out)
{
	int rc = 0;

	k_mutex_lock(&data_lock, K_FOREVER);
	if (latest_valid) {
		*out = latest;
	} else {
		rc = -ENODATA;
	}
	k_mutex_unlock(&data_lock);
	return rc;
}

int modbus_client_set_coil(uint16_t coil, bool on)
{
	int rc;

	k_mutex_lock(&txn_lock, K_FOREVER);
	rc = connect_locked();
	if (rc == 0) {
		rc = modbus_write_coil(iface, MODBUS_UNIT_ID, coil, on);
	}
	k_mutex_unlock(&txn_lock);
	return rc;
}

int modbus_client_initialize(void)
{
	const struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RAW,
		.rx_timeout = MODBUS_RX_TIMEOUT_US,
		.rawcb = { .raw_tx_cb = raw_tx_cb, .user_data = NULL },
	};
	int rc;

	iface = modbus_iface_get_by_name("RAW_0");
	if (iface < 0) {
		LOG_ERR("Modbus RAW_0 interface not found: %d", iface);
		return iface;
	}

	rc = modbus_init_client(iface, param);
	if (rc) {
		LOG_ERR("modbus_init_client failed: %d", rc);
		return rc;
	}

	k_thread_create(&poll_thread_data, poll_stack, K_THREAD_STACK_SIZEOF(poll_stack),
			poll_fn, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&poll_thread_data, "modbus_poll");
	return 0;
}