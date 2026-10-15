/*
 * mbedTLS USER config for the DESKTOP build (Linux/Windows CI and the pairing CLI). It is included AFTER the stock
 * mbedtls_config.h (via -DMBEDTLS_USER_CONFIG_FILE), so it only ADDS the DTLS-SRTP and X.509-writing features the
 * WebRTC media transport needs. Unlike the iOS 6 config it keeps mbedTLS's own entropy/time - no platform glue.
 */
#ifndef WA_MBEDTLS_CONFIG_HOST_H
#define WA_MBEDTLS_CONFIG_HOST_H

/* WebRTC media transport */
#ifndef MBEDTLS_SSL_PROTO_DTLS
#define MBEDTLS_SSL_PROTO_DTLS
#endif
#define MBEDTLS_SSL_DTLS_SRTP

/* Self-signed DTLS certificate written on the fly */
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_PK_WRITE_C

#endif
