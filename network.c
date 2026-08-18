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
    // The PSP Netconf Utility handles all memory initialization automatically!
    // Manually calling sceNetInit here causes a double-allocation kernel panic.
    return 0;
}

int connect_to_ap(void) {
    pspUtilityNetconfData data;
    memset(&data, 0, sizeof(data));
    data.base.size = sizeof(data);
    data.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    data.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
    data.base.graphicsThread = 17;
    data.base.accessThread = 19;
    data.base.fontThread = 18;
    data.base.soundThread = 16;
    data.action = PSP_NETCONF_ACTION_CONNECTAP;
    
    struct pspUtilityNetconfAdhoc adhocparam;
    memset(&adhocparam, 0, sizeof(adhocparam));
    data.adhocparam = &adhocparam;

    if (sceUtilityNetconfInitStart(&data) != 0) return -1;

    int running = 1;
    while (running) {
        int status = sceUtilityNetconfGetStatus();
        
        if (status == PSP_UTILITY_DIALOG_NONE) {
            running = 0;
        } else if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            // Mode 2 lets the firmware handle the graphics completely!
            sceUtilityNetconfUpdate(2); 
        } else if (status == PSP_UTILITY_DIALOG_QUIT) {
            sceUtilityNetconfShutdownStart();
        }
        
        sceDisplayWaitVblankStart();
    }

    int state = 0;
    sceNetApctlGetState(&state);
    return (state == 4) ? 0 : -2;
}

void terminate_networking(void) {
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}
