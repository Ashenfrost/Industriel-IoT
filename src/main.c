#include <zephyr/kernel.h>
#include "network.h"

int main(void)
{
    printk("Zephyr app started\n");
    network_init();

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}
