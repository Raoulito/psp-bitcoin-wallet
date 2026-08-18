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
    int state = 0;
    
    int err = sceNetApctlConnect(1);
    if (err != 0) {
        return err;
    }

    // Wait for the connection to establish (max 30 seconds)
    int timeout = 60; 
    while (timeout > 0) {
        if (sceNetApctlGetState(&state) != 0) return -3;
        
        if (state == 4) { // PSP_NET_APCTL_STATE_GOT_IP
            return 0;
        }
        
        // If state == 0 (disconnected) after we started connecting, it failed the handshake
        if (state == 0 && timeout < 55) {
            return -100;
        }
        
        sceKernelDelayThread(500 * 1000); // Wait 500ms
        timeout--;
    }

    return -200 - state; // Timeout, return state
}

void terminate_networking(void) {
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}
