#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_netmodules.h>
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

    sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    sceNetInetInit();
    sceNetResolverInit();
    sceNetApctlInit(0x8000, 48);

    initialized = 1;
    return 0;
}

int connect_to_ap(void) {
    int state = 0;
    
    int err = sceNetApctlConnect(1);
    if (err != 0) {
        return err;
    }

    // Wait for the connection to establish (max 15 seconds)
    int timeout = 30; 
    while (timeout > 0) {
        if (sceNetApctlGetState(&state) != 0) return -3;
        
        if (state == 4) { // PSP_NET_APCTL_STATE_GOT_IP
            return 0;
        }
        sceKernelDelayThread(500 * 1000); // Wait 500ms
        timeout--;
    }

    return -2; // Timeout
}

void terminate_networking(void) {
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}
