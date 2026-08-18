#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

// Generic HTTPS request function using mbedTLS and PSP sockets
int https_request(const char *method, const char *host, const char *path, const char *body, char *response_buf, size_t response_max);

// Connects to WiFi, establishes a TLS connection to mempool.space, and fetches the balance in satoshis
int fetch_bitcoin_balance(const char *address, int is_testnet, uint64_t *balance_sats);

#endif
