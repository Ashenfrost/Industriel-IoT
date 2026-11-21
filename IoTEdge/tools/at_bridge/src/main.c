/* AT bridge: everything typed on the USB console goes to the modem (UART1),
 * everything the modem answers goes back to the USB console.
 * Test tool only - lets the PC talk AT directly to the SIM7080G. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

static const struct device *const host = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *const modem = DEVICE_DT_GET(DT_NODELABEL(uart1));

int main(void)
{
	unsigned char c;

	if (!device_is_ready(host) || !device_is_ready(modem)) {
		printk("AT bridge: UART not ready\n");
		return 0;
	}
	printk("AT bridge ready (UART1 TX=GPIO23 RX=GPIO22 115200)\n");

	while (1) {
		bool idle = true;

		while (uart_poll_in(host, &c) == 0) {
			uart_poll_out(modem, c);
			idle = false;
		}
		while (uart_poll_in(modem, &c) == 0) {
			uart_poll_out(host, c);
			idle = false;
		}
		if (idle) {
			k_usleep(50); /* ~0.6 byte at 115200, UART FIFO easily covers it */
		}
	}
	return 0;
}
