#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tx_builder.h"
#include "http.h"
#include "base58.h"
#include "sha2.h"
#include "ecdsa.h"
#include "secp256k1.h"
#include "hasher.h"

size_t address_to_scriptpubkey(const char *address, int is_testnet, uint8_t *script) {
    uint8_t decoded[32];
    int dec_len = base58_decode_check(address, HASHER_SHA2D, decoded, sizeof(decoded));
    uint8_t expected_version = is_testnet ? 0x6F : 0x00;
    if (dec_len != 21 || decoded[0] != expected_version) return 0;
    
    script[0] = 0x76; // OP_DUP
    script[1] = 0xA9; // OP_HASH160
    script[2] = 0x14; // Push 20 bytes
    memcpy(script + 3, decoded + 1, 20);
    script[23] = 0x88; // OP_EQUALVERIFY
    script[24] = 0xAC; // OP_CHECKSIG
    return 25;
}

int fetch_utxos(const char *address, int is_testnet, UTXO *utxos, int *utxo_count) {
    char path[256];
    if (is_testnet) {
        snprintf(path, sizeof(path), "/testnet/api/address/%s/utxo", address);
    } else {
        snprintf(path, sizeof(path), "/api/address/%s/utxo", address);
    }
    
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
        
        memcpy(utxos[*utxo_count].txid_hex, ptr, 64);
        utxos[*utxo_count].txid_hex[64] = 0;
        
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

int broadcast_tx(const char *raw_tx_hex, int is_testnet, char *txid_out, size_t txid_out_size) {
    char resp[1024];
    const char *path = is_testnet ? "/testnet/api/tx" : "/api/tx";
    if (https_request("POST", "mempool.space", path, raw_tx_hex, resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        strncpy(txid_out, body, txid_out_size - 1);
        txid_out[txid_out_size - 1] = '\0';
        return 0;
    }
    return -2;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuffer;

static void append_u8(ByteBuffer *b, uint8_t v) { if (b->len < b->cap) b->data[b->len++] = v; }
static void append_u32_le(ByteBuffer *b, uint32_t v) {
    append_u8(b, v & 0xFF);
    append_u8(b, (v >> 8) & 0xFF);
    append_u8(b, (v >> 16) & 0xFF);
    append_u8(b, (v >> 24) & 0xFF);
}
static void append_u64_le(ByteBuffer *b, uint64_t v) {
    append_u32_le(b, v & 0xFFFFFFFF);
    append_u32_le(b, (v >> 32) & 0xFFFFFFFF);
}
static void append_varint(ByteBuffer *b, uint64_t v) {
    if (v < 0xfd) {
        append_u8(b, (uint8_t)v);
    } else if (v <= 0xffff) {
        append_u8(b, 0xfd);
        append_u8(b, v & 0xFF);
        append_u8(b, (v >> 8) & 0xFF);
    } else if (v <= 0xffffffff) {
        append_u8(b, 0xfe);
        append_u32_le(b, (uint32_t)v);
    } else {
        append_u8(b, 0xff);
        append_u64_le(b, v);
    }
}
static void append_bytes(ByteBuffer *b, const uint8_t *bytes, size_t len) {
    for (size_t i=0; i<len; i++) append_u8(b, bytes[i]);
}

static void build_tx(ByteBuffer *b, UTXO *utxos, int num_inputs, uint64_t amount, uint64_t change, const uint8_t *dest_script, size_t dest_len, const uint8_t *change_script, size_t change_len, int sign_index) {
    append_u32_le(b, 1); // Version 1
    
    append_varint(b, num_inputs);
    for (int i = 0; i < num_inputs; i++) {
        append_bytes(b, utxos[i].txid_bytes, 32);
        append_u32_le(b, utxos[i].vout);
        
        if (i == sign_index) {
            append_varint(b, change_len);
            append_bytes(b, change_script, change_len);
        } else {
            append_varint(b, 0); // Empty scriptSig
        }
        
        append_u32_le(b, 0xFFFFFFFF); // Sequence
    }
    
    int num_outputs = (change > 0) ? 2 : 1;
    append_varint(b, num_outputs);
    
    // Output 1: Destination
    append_u64_le(b, amount);
    append_varint(b, dest_len);
    append_bytes(b, dest_script, dest_len);
    
    // Output 2: Change (if any)
    if (change > 0) {
        append_u64_le(b, change);
        append_varint(b, change_len);
        append_bytes(b, change_script, change_len);
    }
    
    append_u32_le(b, 0); // Locktime 0
    
    if (sign_index >= 0) {
        append_u32_le(b, 1); // SIGHASH_ALL
    }
}

int send_bitcoin(HDNode *node, const char *src_address, const char *dest_address, uint64_t amount_sats, int is_testnet, char *txid_out, size_t txid_out_size) {
    UTXO utxos[MAX_UTXOS];
    int utxo_count = 0;
    
    if (fetch_utxos(src_address, is_testnet, utxos, &utxo_count) != 0) return -1;
    if (utxo_count == 0) return -2;
    
    uint8_t dest_script[32];
    size_t dest_len = address_to_scriptpubkey(dest_address, is_testnet, dest_script);
    if (dest_len == 0) return -3;
    
    uint8_t change_script[32];
    size_t change_len = address_to_scriptpubkey(src_address, is_testnet, change_script);
    if (change_len == 0) return -4;
    
    uint64_t selected_val = 0;
    uint64_t fee = 500; 
    int inputs_to_use = 0;
    
    if (amount_sats == 0) { // sweep
        for (int i=0; i<utxo_count; i++) selected_val += utxos[i].value;
        inputs_to_use = utxo_count;
        if (selected_val <= fee) return -5;
        amount_sats = selected_val - fee;
    } else {
        for (int i=0; i<utxo_count; i++) {
            selected_val += utxos[i].value;
            inputs_to_use++;
            if (selected_val >= amount_sats + fee) break;
        }
        if (selected_val < amount_sats + fee) return -6; // Insufficient funds
    }
    
    uint64_t change = selected_val - amount_sats - fee;
    
    uint8_t signatures[MAX_UTXOS][75]; // DER signatures
    size_t sig_lens[MAX_UTXOS];
    
    uint8_t buf[4096];
    ByteBuffer b;
    
    for (int i = 0; i < inputs_to_use; i++) {
        b.data = buf; b.len = 0; b.cap = sizeof(buf);
        build_tx(&b, utxos, inputs_to_use, amount_sats, change, dest_script, dest_len, change_script, change_len, i);
        
        uint8_t hash[32];
        hasher_Raw(HASHER_SHA2D, b.data, b.len, hash);
        
        uint8_t sig[64];
        uint8_t pby;
        if (ecdsa_sign_digest(&secp256k1, node->private_key, hash, sig, &pby, NULL) != 0) return -7;
        
        int der_len = ecdsa_sig_to_der(sig, signatures[i]);
        signatures[i][der_len] = 0x01; // SIGHASH_ALL byte appended to signature
        sig_lens[i] = der_len + 1;
    }
    
    // Assemble final transaction
    b.data = buf; b.len = 0; b.cap = sizeof(buf);
    append_u32_le(&b, 1);
    append_varint(&b, inputs_to_use);
    for (int i = 0; i < inputs_to_use; i++) {
        append_bytes(&b, utxos[i].txid_bytes, 32);
        append_u32_le(&b, utxos[i].vout);
        
        size_t script_sig_len = 1 + sig_lens[i] + 1 + 33; 
        append_varint(&b, script_sig_len);
        append_u8(&b, sig_lens[i]);
        append_bytes(&b, signatures[i], sig_lens[i]);
        append_u8(&b, 33);
        append_bytes(&b, node->public_key, 33);
        
        append_u32_le(&b, 0xFFFFFFFF);
    }
    
    int num_outputs = (change > 0) ? 2 : 1;
    append_varint(&b, num_outputs);
    append_u64_le(&b, amount_sats);
    append_varint(&b, dest_len);
    append_bytes(&b, dest_script, dest_len);
    
    if (change > 0) {
        append_u64_le(&b, change);
        append_varint(&b, change_len);
        append_bytes(&b, change_script, change_len);
    }
    append_u32_le(&b, 0); // Locktime
    
    char *hex = malloc(b.len * 2 + 1);
    if (!hex) return -8;
    for (size_t i = 0; i < b.len; i++) {
        sprintf(&hex[i*2], "%02x", b.data[i]);
    }
    
    int ret = broadcast_tx(hex, is_testnet, txid_out, txid_out_size);
    free(hex);
    return ret;
}
