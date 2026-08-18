#include <pspkernel.h>
#include <pspnet_resolver.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

#include "network.h"
#include "http.h"

static int psp_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *((int*)ctx);
    int ret = send(fd, buf, len, 0);
    if (ret < 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return ret;
}

static int psp_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *((int*)ctx);
    int ret = recv(fd, buf, len, 0);
    if (ret < 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return ret;
}

int fetch_bitcoin_balance(const char *address, uint64_t *balance_sats) {
    *balance_sats = 0;

    // 1. Resolve DNS for mempool.space
    char resolver_buf[1024];
    int rid;
    if (sceNetResolverCreate(&rid, resolver_buf, sizeof(resolver_buf)) < 0) return -1;
    
    struct in_addr addr;
    if (sceNetResolverStartNtoA(rid, "mempool.space", &addr, 5, 3) < 0) {
        sceNetResolverDelete(rid);
        return -2;
    }
    sceNetResolverDelete(rid);

    // 2. Open standard TCP Socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -3;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(443); // HTTPS
    dest.sin_addr = addr;

    if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        close(sock);
        return -4;
    }

    // 3. Initialize mbedTLS
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "psp_btc_wallet";
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));

    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    
    // We skip certificate verification because the PSP doesn't have a modern CA root store built-in
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE); 
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, "mempool.space"); // SNI
    
    // Wire up our custom PSP socket callbacks!
    mbedtls_ssl_set_bio(&ssl, &sock, psp_send, psp_recv, NULL);

    // Perform TLS handshake
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto cleanup; // Handshake failed
        }
    }

    // 4. Send HTTP GET Request
    char req[512];
    snprintf(req, sizeof(req), 
        "GET /api/address/%s HTTP/1.1\r\n"
        "Host: mempool.space\r\n"
        "User-Agent: PSP-Bitcoin-Wallet/1.0\r\n"
        "Connection: close\r\n\r\n", address);

    mbedtls_ssl_write(&ssl, (const unsigned char*)req, strlen(req));

    // 5. Read HTTP Response
    char resp[4096];
    memset(resp, 0, sizeof(resp));
    int read_len;
    int total_read = 0;
    while ((read_len = mbedtls_ssl_read(&ssl, (unsigned char*)(resp + total_read), sizeof(resp) - total_read - 1)) > 0) {
        total_read += read_len;
        if (total_read >= sizeof(resp) - 1) break;
    }

    // 6. Quick and Dirty JSON Parsing
    // We search for "funded_txo_sum": X and "spent_txo_sum": Y
    char *funded = strstr(resp, "\"funded_txo_sum\":");
    char *spent = strstr(resp, "\"spent_txo_sum\":");
    
    if (funded && spent) {
        uint64_t f_sum = strtoull(funded + 17, NULL, 10);
        uint64_t s_sum = strtoull(spent + 16, NULL, 10);
        *balance_sats = f_sum - s_sum;
    }

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    close(sock);

    return (funded && spent) ? 0 : -5;
}
