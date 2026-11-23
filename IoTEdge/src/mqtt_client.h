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

#include <zephyr/kernel.h>

/* Deep sleep design (APP_DEEP_SLEEP): the MQTT thread has no schedule of its
 * own. This asks it to run one session now with everything queued, and
 * waits for the result. Returns 0 when every queued sample was sent, or a
 * negative errno (-EAGAIN on timeout). */
int mqtt_client_send_now(k_timeout_t timeout);

/* Samples waiting in the queue */
int mqtt_client_queued(void);
