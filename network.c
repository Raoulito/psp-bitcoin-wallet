#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspsdk.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <psputility_netconf.h>

#include "network.h"
#include "graphics.h"

int init_networking(void) {
    static int initialized = 0;
    if (initialized) return 0;
    
    sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

    int err = pspSdkInetInit();
    if (err != 0) {
        return err;
    }

    initialized = 1;
    return 0;
}

int connect_to_ap(void) {
    int err = sceNetApctlConnect(1);
    if (err != 0) {
        return err;
    }
    return 0;
}

void terminate_networking(void) {
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}
