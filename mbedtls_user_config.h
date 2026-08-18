#ifndef MBEDTLS_USER_CONFIG_H
#define MBEDTLS_USER_CONFIG_H

// The PSP does not have standard Unix/Windows entropy sources (like /dev/urandom)
#define MBEDTLS_NO_PLATFORM_ENTROPY

// We will provide our own hardware-backed entropy from the PSP's RTC
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// We will implement custom socket send/recv callbacks for mbedtls later
// We will implement custom socket send/recv callbacks for mbedtls later
// We will implement custom socket send/recv callbacks for mbedtls later
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

#endif // MBEDTLS_USER_CONFIG_H
