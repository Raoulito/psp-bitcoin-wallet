TARGET_PSP = 1

TARGET = psp-bitcoin-wallet

# Main project files
OBJS = main.o graphics.o rand_psp.o network.o http.o tx_builder.o

# Trezor-crypto objects
CRYPTO_SRCS = $(wildcard trezor-crypto/*.c)
CRYPTO_DONNA = $(wildcard trezor-crypto/ed25519-donna/*.c)
CRYPTO_AES = $(wildcard trezor-crypto/aes/*.c)
CRYPTO_CHACHA = $(wildcard trezor-crypto/chacha20poly1305/*.c)

# mbedTLS objects
MBEDTLS_SRCS = $(wildcard mbedtls/library/*.c)
MBEDTLS_OBJS = $(patsubst %.c,%.o,$(MBEDTLS_SRCS))
OBJS += $(MBEDTLS_OBJS)

INCDIR = trezor-crypto mbedtls/include

CRYPTO_OBJS = $(patsubst %.c,%.o,$(CRYPTO_SRCS)) \
              $(patsubst %.c,%.o,$(CRYPTO_DONNA)) \
              $(patsubst %.c,%.o,$(CRYPTO_AES)) \
              $(patsubst %.c,%.o,$(CRYPTO_CHACHA))

# Exclude rand_insecure as we are providing our own rand_psp
# Exclude zkp_* because they require the secp256k1-zkp submodule which is not initialized
# Exclude aes/aestst.o because it contains x86-specific assembly (rdtsc)
CRYPTO_OBJS := $(filter-out trezor-crypto/rand_insecure.o trezor-crypto/zkp_%.o trezor-crypto/aes/aestst.o, $(CRYPTO_OBJS))

OBJS += $(CRYPTO_OBJS)

INCDIR = trezor-crypto mbedtls/include
CFLAGS = -O2 -G0 -Wall -Wno-unused-function -D_PSP_ \
         -DUSE_KECCAK=1 -DUSE_NEM=1 -DUSE_CARDANO=1 \
         -DAES_128 -DAES_192 -DAES_VAR \
         -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\"
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

# Build a relocatable PRX rather than a static ELF.
#
# Required for the 0x0800 module attribute in main.c to have any effect: a static
# ELF is loaded as a plain user module and its attribute field is ignored, which
# is why sceUtilityNetconfInitStart() and the sceUtility*NetParam calls returned
# 0x8002013A (LIBRARY_NOT_YET_LINKED). CMFileManager-PSP ships a single EBOOT
# built this way (BUILD_PRX=1 + attribute 0x800).
BUILD_PRX = 1

LIBDIR =
LDFLAGS =
# Do NOT repeat -lpspnet / -lpspnet_apctl / -lpspdebug / -lpspdisplay / -lpspge /
# -lpspctrl here: build.mak appends them after this list. Listing a stub library
# twice splits its .lib.stub section, which makes psp-fixup-imports warn
# "stubs out of order" and skip merging that library's import entries.
LIBS = -lintrafont -lpspgum -lpspgu -lm -lpspwlan -lpspnet_resolver \
       -lpspnet_inet -lpsputility -lpsppower -lpsprtc -lpspsdk

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Bitcoin Wallet

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
