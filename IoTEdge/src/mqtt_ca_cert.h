/* mqtt_ca_cert.h - CA certificate the broker's TLS certificate is checked
 * against (used when APP_MQTT_TLS = 1). Not a secret: it is public, so this
 * file is meant to be committed.
 *
 * Paste the PEM as one C string, every line ending in \n:
 *   #define MQTT_CA_CERT_PEM \
 *       "-----BEGIN CERTIFICATE-----\n" \
 *       "MIID...\n" \
 *       "-----END CERTIFICATE-----\n"
 *
 * Empty = TLS without checking the server's certificate. The traffic is
 * still encrypted, but a fake broker would not be detected. Only meant as a
 * stop-gap until issue #22 sets up the broker's certificate.
 */
#pragma once

#define MQTT_CA_CERT_PEM ""
