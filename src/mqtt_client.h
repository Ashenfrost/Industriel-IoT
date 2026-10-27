/* mqtt_client.h */
#pragma once

#include "sparkplug.h"

/* Starts the MQTT thread. Every APP_PUBLISH_INTERVAL_S it connects, sends
 * NBIRTH + all queued samples as NDATA + NDEATH, and disconnects again. */
int mqtt_client_initialize(void);

/* Hands a sample to the MQTT thread. Safe to call from any thread.
 * If the queue is full (broker unreachable for a long time) the oldest
 * sample is dropped to make room. */
int mqtt_client_queue_sample(const struct sparkplug_sample *sample);
