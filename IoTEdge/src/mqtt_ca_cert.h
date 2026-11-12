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
 * still encrypted, but a fake broker would not be detected.
 *
 * The broker's certificate (CN and SAN = the broker IP, valid 2 years) is
 * signed by this CA. When it expires, sign a new one with the same CA: the
 * devices keep working without a firmware update. Only a new CA needs a
 * reflash of every device.
 */
#pragma once

/* UCL IoT Group CA (RSA 2048, valid to 2036-11-08). Signs the broker's
 * certificate for its TLS port (8883). The CA's private key is kept offline. */
#define MQTT_CA_CERT_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDQDCCAiigAwIBAgIUW+Hniwuko8MKF/QFyfWHhj49lTYwDQYJKoZIhvcNAQEL\n" \
    "BQAwODEZMBcGA1UEAwwQVUNMIElvVCBHcm91cCBDQTEbMBkGA1UECgwSVUNMIElu\n" \
    "ZHVzdHJpZWwgSW9UMB4XDTI2MTExMTExMTQ0MFoXDTM2MTEwODExMTQ0MFowODEZ\n" \
    "MBcGA1UEAwwQVUNMIElvVCBHcm91cCBDQTEbMBkGA1UECgwSVUNMIEluZHVzdHJp\n" \
    "ZWwgSW9UMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA213p8d3G2VSU\n" \
    "TxmZ6LhhKRsWUBM2owyHSbyHgqfsjSDSoACwoe2aD9sSxwmp/nFpoD3XdJ4Hnhpu\n" \
    "AzhX4NnuwRFQY42fSAesXMvPuPI97UoFFPY1tD3RfFm0Pwdda7kyo6ZadWqnqJk6\n" \
    "S1ZLU7rCA/2fzHS7IbfVvlKExMYeMuYJtruMF254OtkIxNUKVE4wandGLvMgqiTx\n" \
    "b6QdcmMFMooDq8PVcbQbXQqkKMW4a0oNS9TSa+x//pzGtSvO7ZdLT9iP7/LerDvB\n" \
    "YY55zZu2ghGNVqp+eJF4fawJJqLvzhfGq9JN0psYY1EJ8RzgqlEQ/C+1n9OZIxkN\n" \
    "v59JMZ59XQIDAQABo0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIB\n" \
    "BjAdBgNVHQ4EFgQUSanyTYyJAILAkh2SVJEoYWawpdgwDQYJKoZIhvcNAQELBQAD\n" \
    "ggEBAJhYRi6DJe2NG9yAD0X3Jekn6DK9zLhkIU0yLeLq3cKD6eFeXtpsZLzM3jrk\n" \
    "WIWMTiEmv0tUndqHafD+g7UhMKRKiyJdl3ggMP+mE6wRRfWrf2UMgRqMDEejVQw1\n" \
    "sWs1HPnRZ/qb7q3YEacYpWZeS2B61oQMfQvuBlaXaKYJczeH4l4eEigc6PIoKKv5\n" \
    "ile/PtSlq+iOn171lVBeTBADSMygSOzHRC+EiJY8eSTLTMXUWeXtEeS2YE+7hhbA\n" \
    "COAd0yG4dfNVEgf5lz+676ou+ftwuf71Xlxc8xoNxeDEdyjhFXjBzM6aS1md87GE\n" \
    "+Q6Oc/AkPvfHzaz1cPATl+xUgP0=\n" \
    "-----END CERTIFICATE-----\n" \
    ""

