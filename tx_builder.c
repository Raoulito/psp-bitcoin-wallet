#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tx_builder.h"
#include "http.h"
#include "base58.h"
#include "sha2.h"
#include "ecdsa.h"
#include "secp256k1.h"

int fetch_utxos(const char *address, UTXO *utxos, int *utxo_count) {
    char path[256];
    snprintf(path, sizeof(path), "/api/address/%s/utxo", address);
    
    char resp[8192];
    if (https_request("GET", "mempool.space", path, NULL, resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    char *ptr = resp;
    *utxo_count = 0;
    while (*utxo_count < MAX_UTXOS) {
        ptr = strstr(ptr, "\"txid\":\"");
        if (!ptr) break;
        ptr += 8;
        
        // Copy 64 hex chars
        memcpy(utxos[*utxo_count].txid_hex, ptr, 64);
        utxos[*utxo_count].txid_hex[64] = 0;
        
        // Convert to bytes (reversed for raw transaction inputs)
        for (int i=0; i<32; i++) {
            char byte_str[3] = {utxos[*utxo_count].txid_hex[i*2], utxos[*utxo_count].txid_hex[i*2+1], 0};
            utxos[*utxo_count].txid_bytes[31-i] = (uint8_t)strtoul(byte_str, NULL, 16);
        }
        
        ptr = strstr(ptr, "\"vout\":");
        if (!ptr) break;
        ptr += 7;
        utxos[*utxo_count].vout = strtoul(ptr, NULL, 10);
        
        ptr = strstr(ptr, "\"value\":");
        if (!ptr) break;
        ptr += 8;
        utxos[*utxo_count].value = strtoull(ptr, NULL, 10);
        
        (*utxo_count)++;
    }
    return 0;
}

int broadcast_tx(const char *raw_tx_hex, char *txid_out, size_t txid_out_size) {
    char resp[1024];
    if (https_request("POST", "mempool.space", "/api/tx", raw_tx_hex, resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    // mempool.space returns the txid in the body (or an error message)
    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        strncpy(txid_out, body, txid_out_size - 1);
        txid_out[txid_out_size - 1] = '\0';
        return 0;
    }
    return -2;
}

int send_bitcoin(HDNode *node, const char *src_address, const char *dest_address, uint64_t amount_sats, char *txid_out, size_t txid_out_size) {
    UTXO utxos[MAX_UTXOS];
    int utxo_count = 0;
    
    // 1. Fetch UTXOs for our address
    if (fetch_utxos(src_address, utxos, &utxo_count) != 0) return -1;
    if (utxo_count == 0) return -2; // No funds
    
    // 2. Select UTXOs to cover amount_sats + fee
    uint64_t selected_val = 0;
    uint64_t fee = 500; // Hardcoded fallback fee for a simple 1-in 2-out tx
    int inputs_to_use = 0;
    
    if (amount_sats == 0) { // Sweep wallet
        for (int i=0; i<utxo_count; i++) selected_val += utxos[i].value;
        inputs_to_use = utxo_count;
        amount_sats = selected_val - fee;
    } else {
        for (int i=0; i<utxo_count; i++) {
            selected_val += utxos[i].value;
            inputs_to_use++;
            if (selected_val >= amount_sats + fee) break;
        }
        if (selected_val < amount_sats + fee) return -3; // Insufficient funds
    }
    
    // 3. Assemble and Sign the Raw Transaction Byte-by-Byte
    // TODO: This requires ~200 lines of raw byte serialization (VarInts, scriptPubKey P2PKH generation)
    // and passing SIGHASH_ALL to trezor-crypto's ecdsa_sign_digest.
    
    // 4. Broadcast
    // return broadcast_tx(raw_tx_hex, txid_out, txid_out_size);
    
    return -99; // Not implemented yet
}
