/* mqtt_client.h */
#pragma once

#include <stdbool.h>
#include "sparkplug.h"

/* Starts the MQTT thread. It connects (and reconnects) on its own, sends
 * NBIRTH on every new session and publishes queued samples as NDATA. */
int mqtt_client_initialize(void);

/* Hands a sample to the MQTT thread. Safe to call from any thread.
 * If the queue is full (broker unreachable for a long time) the oldest
 * sample is dropped to make room. */
int mqtt_client_queue_sample(const struct sparkplug_sample *sample);

bool mqtt_client_is_connected(void);
