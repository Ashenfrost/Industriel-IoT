/* app_config.h - tunables for sampling, batching and Sparkplug naming.
 *
 * Everything that changes the data/power trade-off lives here, so it can be
 * tuned in one place (and argued for in the report).
 */
#pragma once

/* Sparkplug B group id: first level after "spBv1.0/" in every topic.
 * Keep it short - it is sent in every single topic string. */
#ifndef SPARKPLUG_GROUP_ID
#define SPARKPLUG_GROUP_ID "UCL"
#endif

/* How often the application thread reads the sensor (kravspec: 5-15 s). */
#define APP_SAMPLE_INTERVAL_S   10

/* How many samples go into one NDATA message. 1 = send every sample,
 * higher = fewer, larger messages (less protocol overhead per sample). */
#define APP_PUBLISH_BATCH_SIZE  6

/* Samples kept while the broker is unreachable. When full, the oldest
 * sample is dropped. Must be >= APP_PUBLISH_BATCH_SIZE. */
#define APP_SAMPLE_QUEUE_LEN    60

/* SNTP server used to get real UTC time for Sparkplug timestamps. */
#define APP_SNTP_SERVER         "pool.ntp.org"
