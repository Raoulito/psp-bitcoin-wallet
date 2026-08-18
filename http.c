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
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"

#include "network.h"
#include "http.h"

/* Per-socket timeout for a single blocking recv/send. */
#define HTTP_IO_TIMEOUT_SEC   8
/* Hard ceiling for one request, handshake included. */
#define HTTP_TOTAL_TIMEOUT_US (40 * 1000 * 1000)
/* Guards against a hot spin when the socket fails immediately every time. */
#define BIO_MAX_FAIL_STREAK   64

typedef struct {
    int fd;
    SceInt64 deadline; /* sceKernelGetSystemTimeWide() value after which we bail */
    int fail_streak;  /* consecutive failed recv/send calls */
} psp_bio_ctx;

/*
 * The previous version mapped *every* negative return to WANT_WRITE/WANT_READ.
 * mbedtls_ssl_handshake() retries WANT_* forever, so a reset or dead connection
 * froze the wallet instead of returning an error.
 *
 * We deliberately do not inspect errno: the PSP maps sceNetInetGetErrno through
 * cglue and the value space is not worth relying on. Instead retrying is bounded
 * twice over - by the request deadline and by a consecutive-failure count - so
 * these callbacks always terminate.
 */
static int bio_expired(const psp_bio_ctx *ctx)
{
    return sceKernelGetSystemTimeWide() > ctx->deadline;
}

static int bio_retry_or_fail(psp_bio_ctx *ctx, int want, int fail)
{
    if (bio_expired(ctx)) return fail;
    if (++ctx->fail_streak > BIO_MAX_FAIL_STREAK) return fail;
    sceKernelDelayThread(10 * 1000);   /* don't burn the CPU while retrying */
    return want;
}

static int psp_send(void *p, const unsigned char *buf, size_t len)
{
    psp_bio_ctx *ctx = (psp_bio_ctx *)p;
    int ret = send(ctx->fd, buf, len, 0);

    if (ret >= 0) {
        ctx->fail_streak = 0;
        return ret;
    }
    return bio_retry_or_fail(ctx, MBEDTLS_ERR_SSL_WANT_WRITE, MBEDTLS_ERR_NET_SEND_FAILED);
}

static int psp_recv(void *p, unsigned char *buf, size_t len)
{
    psp_bio_ctx *ctx = (psp_bio_ctx *)p;
    int ret = recv(ctx->fd, buf, len, 0);

    /* 0 is an orderly close; mbedtls turns it into MBEDTLS_ERR_SSL_CONN_EOF. */
    if (ret >= 0) {
        ctx->fail_streak = 0;
        return ret;
    }
    return bio_retry_or_fail(ctx, MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_NET_RECV_FAILED);
}

const char *http_strerror(int err)
{
    switch (err) {
        case 0:                      return "OK";
        case HTTP_ERR_ARGS:          return "bad arguments";
        case HTTP_ERR_RESOLVER:      return "resolver create failed";
        case HTTP_ERR_DNS:           return "DNS lookup failed";
        case HTTP_ERR_SOCKET:        return "socket() failed";
        case HTTP_ERR_CONNECT:       return "TCP connect failed";
        case HTTP_ERR_TLS_SETUP:     return "TLS setup failed";
        case HTTP_ERR_TLS_HANDSHAKE: return "TLS handshake failed";
        case HTTP_ERR_WRITE:         return "send failed";
        case HTTP_ERR_EMPTY:         return "empty response";
        case HTTP_ERR_BAD_RESPONSE:  return "malformed response";
        case HTTP_ERR_PARSE:         return "JSON parse failed";
        case HTTP_ERR_TIMEOUT:       return "timed out";
        default:
            if (err <= -1000) return "HTTP error status";
            return "unknown error";
    }
}

/* Returns the start of the body, or NULL when the header block is incomplete. */
static char *http_body(char *response)
{
    char *sep = strstr(response, "\r\n\r\n");
    if (sep) return sep + 4;
    sep = strstr(response, "\n\n");
    if (sep) return sep + 2;
    return NULL;
}

static int http_status(const char *response)
{
    if (strncmp(response, "HTTP/1.", 7) != 0) return -1;
    /* "HTTP/1.1 200 OK" -> offset 9 */
    return (int)strtol(response + 9, NULL, 10);
}

int https_request(const char *method, const char *host, const char *path,
                  const char *body, char *response_buf, size_t response_max)
{
    char resolver_buf[1024];
    int rid = -1;
    struct in_addr addr;
    struct sockaddr_in dest;
    struct timeval tv;
    psp_bio_ctx bio;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    char req[1024];
    size_t total_read = 0;
    int read_len;
    int status;
    int ret;

    if (!response_buf || response_max < 64) return HTTP_ERR_ARGS;
    memset(response_buf, 0, response_max);

    bio.fd = -1;
    bio.fail_streak = 0;
    bio.deadline = sceKernelGetSystemTimeWide() + HTTP_TOTAL_TIMEOUT_US;

    /* 1. DNS */
    if (sceNetResolverCreate(&rid, resolver_buf, sizeof(resolver_buf)) < 0) {
        return HTTP_ERR_RESOLVER;
    }
    if (sceNetResolverStartNtoA(rid, host, &addr, 5, 3) < 0) {
        sceNetResolverDelete(rid);
        return HTTP_ERR_DNS;
    }
    sceNetResolverDelete(rid);

    /* 2. TCP */
    bio.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bio.fd < 0) return HTTP_ERR_SOCKET;

    /* Without these a stalled AP blocks the render loop indefinitely. */
    tv.tv_sec = HTTP_IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(bio.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(bio.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(443);
    dest.sin_addr = addr;

    if (connect(bio.fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(bio.fd);
        return HTTP_ERR_CONNECT;
    }

    /* 3. TLS */
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    {
        const char *pers = "psp_btc_https";
        ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers));
        if (ret != 0) { ret = HTTP_ERR_TLS_SETUP; goto cleanup; }
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { ret = HTTP_ERR_TLS_SETUP; goto cleanup; }

    /* TODO(security): VERIFY_NONE accepts any certificate, so anything on the
       path can impersonate mempool.space. Pin the CA before using mainnet. */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3); /* TLS 1.2 */

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { ret = HTTP_ERR_TLS_SETUP; goto cleanup; }

    /* Cloudflare-fronted hosts need SNI or the handshake is rejected. */
    ret = mbedtls_ssl_set_hostname(&ssl, host);
    if (ret != 0) { ret = HTTP_ERR_TLS_SETUP; goto cleanup; }

    mbedtls_ssl_set_bio(&ssl, &bio, psp_send, psp_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ret = HTTP_ERR_TLS_HANDSHAKE;
            goto cleanup;
        }
        if (bio_expired(&bio)) { ret = HTTP_ERR_TIMEOUT; goto cleanup; }
    }

    /* 4. Request */
    if (body) {
        snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: PSP-Bitcoin-Wallet/1.0\r\n"
                 "Accept: */*\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n\r\n"
                 "%s", method, path, host, (int)strlen(body), body);
    } else {
        snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: PSP-Bitcoin-Wallet/1.0\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n\r\n", method, path, host);
    }

    {
        size_t sent = 0;
        size_t req_len = strlen(req);
        while (sent < req_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + sent, req_len - sent);
            if (ret > 0) { sent += (size_t)ret; continue; }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                ret = HTTP_ERR_WRITE;
                goto cleanup;
            }
            if (bio_expired(&bio)) { ret = HTTP_ERR_TIMEOUT; goto cleanup; }
        }
    }

    /* 5. Response */
    while (total_read < response_max - 1) {
        read_len = mbedtls_ssl_read(&ssl, (unsigned char *)(response_buf + total_read),
                                    response_max - total_read - 1);
        if (read_len > 0) {
            total_read += (size_t)read_len;
            continue;
        }
        if (read_len == MBEDTLS_ERR_SSL_WANT_READ || read_len == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (bio_expired(&bio)) break;
            continue;
        }
        break; /* CONN_EOF / PEER_CLOSE_NOTIFY / error: body is complete or dead */
    }
    response_buf[total_read] = '\0';

    if (total_read == 0) { ret = HTTP_ERR_EMPTY; goto cleanup; }

    status = http_status(response_buf);
    if (status < 200 || status > 299) {
        ret = (status > 0) ? -(1000 + status) : HTTP_ERR_BAD_RESPONSE;
        goto cleanup;
    }

    ret = 0;

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (bio.fd >= 0) close(bio.fd);

    return ret;
}

int fetch_bitcoin_balance(const char *address, int is_testnet, uint64_t *balance_sats)
{
    char path[256];
    char resp[4096];
    char *bodyp;
    char *funded;
    char *spent;
    int ret;

    if (!balance_sats) return HTTP_ERR_ARGS;
    *balance_sats = 0;

    if (is_testnet) {
        snprintf(path, sizeof(path), "/testnet/api/address/%s", address);
    } else {
        snprintf(path, sizeof(path), "/api/address/%s", address);
    }

    ret = https_request("GET", "mempool.space", path, NULL, resp, sizeof(resp));
    if (ret != 0) return ret;

    bodyp = http_body(resp);
    if (!bodyp) return HTTP_ERR_BAD_RESPONSE;

    /* chain_stats comes before mempool_stats, so the first match is confirmed. */
    funded = strstr(bodyp, "\"funded_txo_sum\":");
    spent  = strstr(bodyp, "\"spent_txo_sum\":");
    if (!funded || !spent) return HTTP_ERR_PARSE;

    {
        uint64_t f_sum = strtoull(funded + 17, NULL, 10);
        uint64_t s_sum = strtoull(spent + 16, NULL, 10);
        *balance_sats = (f_sum >= s_sum) ? (f_sum - s_sum) : 0;
    }
    return 0;
}
