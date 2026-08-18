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

int https_request(const char *method, const char *host, const char *path, const char *body, char *response_buf, size_t response_max) {
    if (!response_buf || response_max == 0) return -1;
    memset(response_buf, 0, response_max);

    // 1. Resolve DNS
    char resolver_buf[1024];
    int rid;
    if (sceNetResolverCreate(&rid, resolver_buf, sizeof(resolver_buf)) < 0) return -1;
    
    struct in_addr addr;
    if (sceNetResolverStartNtoA(rid, host, &addr, 5, 3) < 0) {
        sceNetResolverDelete(rid);
        return -2;
    }
    sceNetResolverDelete(rid);

    // 2. Open TCP Socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -3;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(443);
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

    const char *pers = "psp_btc_https";
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));

    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE); 
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, host); 
    mbedtls_ssl_set_bio(&ssl, &sock, psp_send, psp_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto cleanup;
        }
    }

    // 4. Send HTTP Request
    char req[1024];
    if (body) {
        snprintf(req, sizeof(req), 
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: PSP-Bitcoin-Wallet/1.0\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n"
            "%s", method, path, host, (int)strlen(body), body);
    } else {
        snprintf(req, sizeof(req), 
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: PSP-Bitcoin-Wallet/1.0\r\n"
            "Connection: close\r\n\r\n", method, path, host);
    }

    mbedtls_ssl_write(&ssl, (const unsigned char*)req, strlen(req));

    // 5. Read HTTP Response
    int read_len;
    size_t total_read = 0;
    while ((read_len = mbedtls_ssl_read(&ssl, (unsigned char*)(response_buf + total_read), response_max - total_read - 1)) > 0) {
        total_read += read_len;
        if (total_read >= response_max - 1) break;
    }
    
    ret = 0;

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    close(sock);

    return ret;
}

int fetch_bitcoin_balance(const char *address, int is_testnet, uint64_t *balance_sats) {
    *balance_sats = 0;
    
    char path[256];
    if (is_testnet) {
        snprintf(path, sizeof(path), "/testnet/api/address/%s", address);
    } else {
        snprintf(path, sizeof(path), "/api/address/%s", address);
    }

    char resp[4096];
    if (https_request("GET", "mempool.space", path, NULL, resp, sizeof(resp)) != 0) {
        return -1;
    }

    char *funded = strstr(resp, "\"funded_txo_sum\":");
    char *spent = strstr(resp, "\"spent_txo_sum\":");
    
    if (funded && spent) {
        uint64_t f_sum = strtoull(funded + 17, NULL, 10);
        uint64_t s_sum = strtoull(spent + 16, NULL, 10);
        *balance_sats = f_sum - s_sum;
        return 0;
    }

    return -5;
}
