// mqtt_payload.c
#include "mqtt_payload.h"
#include "client_id.h"

#include <zephyr/kernel.h>
#include <stdio.h>

static char hello_msg[64];
static char plc_msg[128];

const char *mqtt_build_hello_world_message(void)
{
    snprintk(hello_msg, sizeof(hello_msg),
             "hello world from %s", build_client_id_get());
    return hello_msg;
}

const char *mqtt_build_plc_payload_message(void)
{
    snprintk(plc_msg, sizeof(plc_msg),
             "{\"client_id\":\"%s\",\"type\":\"PLC\"}", build_client_id_get());
    return plc_msg;
}
