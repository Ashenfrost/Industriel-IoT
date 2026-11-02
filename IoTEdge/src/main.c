#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "client_id.h"
#include "wifi_network.h"
#include "mqtt_client.h"
#include "ESP32_temperature.h"

int main(void)
{
    printk("Zephyr app started\n");

    build_client_id_initialize();
    printk("client id done\n");

    network_initialize();
    printk("network init done\n");

    mqtt_client_initialize();
    printk("mqtt init done\n");

    //ESP32_temperature_initialize();

    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
