#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "client_id.h"
#include "wifi_network.h"
#include "mqtt_client.h"
#include "ESP32_temperature.h"
#include "modbus_client.h"

int main(void)
{
    printk("Zephyr app started\n");

    build_client_id_initialize();
    printk("client id done\n");

    network_initialize();
    printk("network init done\n");

    mqtt_client_initialize();
    printk("mqtt init done\n");

    modbus_client_initialize();
    printk("modbus init done\n");

    //ESP32_temperature_initialize();

       while (1) {
       struct modbus_readings r;

       if (modbus_client_get_readings(&r) == 0) {
           printk("x10 temp: %d %d %d, status 0x%x, beat %u\n",
                  r.temp_x10[0], r.temp_x10[1], r.temp_x10[2],
                  r.status, r.heartbeat);
       }
       k_sleep(K_SECONDS(5));
   }

    return 0;
}
