/* Public test certificate only: a throw-away self-signed CA ("Wrong Test CA")
 * that must be rejected by our broker's certificate check. Its key is not kept. */
#pragma once

#define TEST_PEM_WRONG \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDETCCAfmgAwIBAgIUaAFYxgxe02CefOELU4MNt8X8EbQwDQYJKoZIhvcNAQEL\n" \
    "BQAwGDEWMBQGA1UEAwwNV3JvbmcgVGVzdCBDQTAeFw0yNjExMTExMDI1NTBaFw0y\n" \
    "NjEyMTExMDI1NTBaMBgxFjAUBgNVBAMMDVdyb25nIFRlc3QgQ0EwggEiMA0GCSqG\n" \
    "SIb3DQEBAQUAA4IBDwAwggEKAoIBAQDVEUhy7oTMaVXS0admNropZENTcB88pji/\n" \
    "ll5r7m3i+mpdmOzvAd7OK6VpzG0yLWgSOYyMp0ZyF8OqqyOKPsUt7w0wWXZrQO7P\n" \
    "W/DgKw+Nsc4cS9RddhpsK7oujEUWWWnLuxW0JjHv+HeJN3lJI7bHkfNQuhgMDYWd\n" \
    "66cR2af/rcrnWAydhy3m0zqbsAa4RHeLFkp5LwD1tPLgzgumsJKFbxR1nP+feovx\n" \
    "f7xOwRDezxTTWhyHZtLHVa8J6IxMMllLzTJq9nSIGM4kg+p3LItJktzzRKfr8lL9\n" \
    "jVmKKpsiARAVt5hY6CdxkLafrOTPCy9EQyDSuNjTQtOX8Yew7bClAgMBAAGjUzBR\n" \
    "MB0GA1UdDgQWBBRrlf9JixBEbNnnwaHfNLqTTVQRsTAfBgNVHSMEGDAWgBRrlf9J\n" \
    "ixBEbNnnwaHfNLqTTVQRsTAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUA\n" \
    "A4IBAQCsRPblgBfYr6yXnqoCbsfWKKaSlJg8rvLmkvKLudkDYTbzcNBOPPr+9Mnw\n" \
    "tWeu/U29VJ/+a00CxpMwsCkfvV3YlHQomrlh6h0ScAEhXOjRZaidukL2icbIZGRL\n" \
    "8MjQ04L8/mu1h94wrIax+pemb6EwfPVVlbVnBCPvXSjqtbgmobPX1SliCnDkz2o7\n" \
    "V4zWvSiWSOdq9Ks4q6jGibM4tkg5vriMrOSrPOoQAW/cpMal9qPWc1I0SaaLefSf\n" \
    "+5rfM8buVoqsgL3oABY+t9cxBZMgTAM75GyAlRVrSO4AZxm7gZroxv8kUEsQg+3S\n" \
    "ksF/E6c4jeTZu8vRqnU+8HH0+sIU\n" \
    "-----END CERTIFICATE-----\n" \
    ""

