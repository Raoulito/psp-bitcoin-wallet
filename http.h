#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

// Connects to WiFi, establishes a TLS connection to mempool.space, and fetches the balance in satoshis
int fetch_bitcoin_balance(const char *address, uint64_t *balance_sats);

#endif
