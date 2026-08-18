#include <stdint.h>
#include <psprtc.h>
#include <sys/time.h>

static uint32_t seed = 0;

void psp_rand_init() {
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    seed = (uint32_t)(tick & 0xFFFFFFFF);
}

// Provided for trezor-crypto
uint32_t random32(void) {
    seed = (1103515245 * seed + 12345);
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    return seed ^ (uint32_t)(tick & 0xFFFFFFFF);
}

void random_buffer(uint8_t *buf, size_t len) {
    uint32_t r = 0;
    for (size_t i = 0; i < len; i++) {
        if (i % 4 == 0) {
            r = random32();
        }
        buf[i] = (r >> ((i % 4) * 8)) & 0xFF;
    }
}

// mbedTLS Hardware Entropy Poll
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    random_buffer((uint8_t*)output, len);
    *olen = len;
    return 0; // 0 indicates success
}
