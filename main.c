#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psphttp.h>
#include <pspssl.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("PSP Bitcoin Wallet", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(20480);

#define printf pspDebugScreenPrintf

/* Exit callback */
int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

/* Callback thread */
int CallbackThread(SceSize args, void *argp) {
    int cbid;
    cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

/* Sets up the callback thread and returns its thread id */
int SetupCallbacks(void) {
    int thid = 0;
    thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}

int main(int argc, char** argv) {
    pspDebugScreenInit();
    SetupCallbacks();

    printf("PSP Bitcoin Wallet\n");
    printf("1. Generate Mnemonic\n");
    printf("2. Import Mnemonic\n");
    printf("3. Check Balance\n");
    printf("4. Send Bitcoin\n");
    printf("5. Receive Bitcoin\n");
    
    while(1) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) {
            break;
        }
        sceDisplayWaitVblankStart();
    }

    sceKernelExitGame();
    return 0;
}
