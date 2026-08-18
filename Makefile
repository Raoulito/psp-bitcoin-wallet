TARGET_PSP = 1

TARGET = psp-bitcoin-wallet
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspwlan -lpspnet_apctl -lpspnet_resolver -lpsputility -lpspnet_inet -lpspnet

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Bitcoin Wallet

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
