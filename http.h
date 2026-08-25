#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stddef.h>

/*
 * https_request() / fetch_bitcoin_balance() error codes.
 * A non-2xx HTTP status is reported as -(1000 + status), e.g. -1404.
 */
#define HTTP_ERR_ARGS           -1
#define HTTP_ERR_RESOLVER       -2
#define HTTP_ERR_DNS            -3
#define HTTP_ERR_SOCKET         -4
#define HTTP_ERR_CONNECT        -5
#define HTTP_ERR_TLS_SETUP      -6
#define HTTP_ERR_TLS_HANDSHAKE  -7
#define HTTP_ERR_WRITE          -8
#define HTTP_ERR_EMPTY          -9
#define HTTP_ERR_BAD_RESPONSE  -10
#define HTTP_ERR_PARSE         -11
#define HTTP_ERR_TIMEOUT       -12

/* Human readable form of the codes above, for on-screen diagnostics. */
const char *http_strerror(int err);

/* Raw mbedTLS error behind the last HTTP_ERR_TLS_* / HTTP_ERR_WRITE failure. */
int http_last_tls_error(void);

/* mbedtls_strerror() text for the above, or "" when there is none. */
const char *http_tls_error_str(void);

/* Free user memory when that error was recorded, to catch allocation failures. */
int http_last_tls_free_kb(void);

/* Generic HTTPS request using mbedTLS over PSP sockets. 0 on success. */
int https_request(const char *method, const char *host, const char *path,
                  const char *body, char *response_buf, size_t response_max);

/* Fetches the confirmed balance in satoshis from mempool.space. 0 on success. */
int fetch_bitcoin_balance(const char *address, int is_testnet, uint64_t *balance_sats);

#endif
