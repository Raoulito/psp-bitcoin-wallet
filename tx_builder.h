#ifndef TX_BUILDER_H
#define TX_BUILDER_H

#include <stdint.h>
#include "bip32.h"

// Max UTXOs to process to avoid running out of memory on the PSP
#define MAX_UTXOS 10

typedef struct {
    char txid_hex[65];
    uint8_t txid_bytes[32]; // Reversed for raw tx
    uint32_t vout;
    uint64_t value;
} UTXO;

// Fetches UTXOs from mempool.space for a given address
int fetch_utxos(const char *address, int is_testnet, UTXO *utxos, int *utxo_count);

// Broadcasts a raw hex transaction to mempool.space
int broadcast_tx(const char *raw_tx_hex, int is_testnet, char *txid_out, size_t txid_out_size);

// Builds, signs, and broadcasts a transaction. 
// If amount_sats == 0, it sweeps the entire wallet balance minus a default fee.
int send_bitcoin(HDNode *node, const char *src_address, const char *dest_address, uint64_t amount_sats, int is_testnet, char *txid_out, size_t txid_out_size);

#endif
