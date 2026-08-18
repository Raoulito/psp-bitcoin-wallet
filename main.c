#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics.h"
#include "intraFont.h"
#include "bip39.h"
#include "bip32.h"
#include "address.h"
#include "curves.h"
#include "rand.h"
#include "network.h"
#include "http.h"
#include "tx_builder.h"

PSP_MODULE_INFO("PSP Bitcoin Wallet", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(20480);

void psp_rand_init();

int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) { sceKernelStartThread(thid, 0, 0); }
    return thid;
}

intraFont* font;

int main(int argc, char** argv) {
    SetupCallbacks();
    initGraphics();
    psp_rand_init();

    intraFontInit();
    // Load standard PSP font from flash
    font = intraFontLoad("flash0:/font/ltn0.pgf", 0);

    char mnemonic[256] = "Press X to generate mnemonic";
    char btc_address[40] = "";
    char balance_str[128] = "Balance: Press [SQUARE] to check";
    char tx_status[128] = "";
    int oldButtons = 0;
    int wifi_connected = 0;
    HDNode global_node;
    int has_node = 0;

    while(1) {
        startFrame();
        clearScreen(0xFF1E1E1E); // Dark gray background

        if (font) {
            intraFontSetStyle(font, 1.0f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 40, "PSP Bitcoin Wallet");

            intraFontSetStyle(font, 0.7f, 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 80, "Mnemonic:");
            
            intraFontSetStyle(font, 0.6f, 0xFFDDDDDD, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 110, "%s", mnemonic);

            intraFontSetStyle(font, 0.7f, 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 150, "Address:");
            
            intraFontSetStyle(font, 0.8f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 180, "%s", btc_address);

            intraFontSetStyle(font, 0.7f, 0xFF00FFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 220, "%s", balance_str);

            intraFontSetStyle(font, 0.7f, 0xFFFFAA00, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 245, "%s", tx_status);

            intraFontSetStyle(font, 0.6f, 0xFFAAAAAA, 0, 0, INTRAFONT_ALIGN_LEFT);
            intraFontPrintf(font, 20, 265, "[X] Gen   [SQUARE] Bal   [TRIANGLE] Sweep   [START] Exit");
        }

        endFrame();

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        
        // Edge detection for buttons
        int btnDown = pad.Buttons & ~oldButtons;
        oldButtons = pad.Buttons;
        
        if (btnDown & PSP_CTRL_START) {
            break;
        }
        if (btnDown & PSP_CTRL_CROSS) {
            // Generate 16 bytes of entropy (128 bits = 12 words)
            uint8_t data[16];
            random_buffer(data, 16);
            const char* generated = mnemonic_from_data(data, 16);
            if (generated) {
                strncpy(mnemonic, generated, sizeof(mnemonic)-1);

                // 1. Convert mnemonic to 64-byte seed
                uint8_t seed[64];
                mnemonic_to_seed(mnemonic, "", seed, 0);

                // 2. Initialize HD node from seed
                hdnode_from_seed(seed, 64, SECP256K1_NAME, &global_node);

                // 3. Derive BIP44 path for Bitcoin: m/44'/0'/0'/0/0
                hdnode_private_ckd(&global_node, 44 | 0x80000000);
                hdnode_private_ckd(&global_node, 0 | 0x80000000);
                hdnode_private_ckd(&global_node, 0 | 0x80000000);
                hdnode_private_ckd(&global_node, 0);
                hdnode_private_ckd(&global_node, 0);
                hdnode_fill_public_key(&global_node);
                has_node = 1;

                ecdsa_get_address(global_node.public_key, 0x00, HASHER_SHA2_RIPEMD, HASHER_SHA2D, btc_address, sizeof(btc_address));
                snprintf(balance_str, sizeof(balance_str), "Balance: Press [SQUARE] to check");
                tx_status[0] = '\0';
            }
        }

        if (btnDown & PSP_CTRL_SQUARE) {
            if (strlen(btc_address) > 0) {
                if (!wifi_connected) {
                    snprintf(balance_str, sizeof(balance_str), "Connecting to WiFi (Profile 1)...");
                    // Force a screen update to show connecting status
                    startFrame(); clearScreen(0xFF1E1E1E); intraFontPrintf(font, 20, 220, "%s", balance_str); endFrame();

                    if (init_networking() == 0 && connect_to_ap(1) == 0) {
                        wifi_connected = 1;
                    }
                }

                if (wifi_connected) {
                    snprintf(balance_str, sizeof(balance_str), "Fetching balance from mempool.space...");
                    startFrame(); clearScreen(0xFF1E1E1E); intraFontPrintf(font, 20, 220, "%s", balance_str); endFrame();

                    uint64_t sats = 0;
                    int ret = fetch_bitcoin_balance(btc_address, &sats);
                    if (ret == 0) {
                        snprintf(balance_str, sizeof(balance_str), "Balance: %llu satoshis", (unsigned long long)sats);
                    } else {
                        snprintf(balance_str, sizeof(balance_str), "Error: HTTP fetch failed (%d)", ret);
                    }
                } else {
                    snprintf(balance_str, sizeof(balance_str), "Error: WiFi connection failed. Check Profile 1.");
                }
            }
        }

        if (btnDown & PSP_CTRL_TRIANGLE) {
            if (has_node && strlen(btc_address) > 0) {
                if (!wifi_connected) {
                    snprintf(tx_status, sizeof(tx_status), "Connecting to WiFi...");
                    startFrame(); clearScreen(0xFF1E1E1E); intraFontPrintf(font, 20, 245, "%s", tx_status); endFrame();
                    if (init_networking() == 0 && connect_to_ap(1) == 0) wifi_connected = 1;
                }

                if (wifi_connected) {
                    snprintf(tx_status, sizeof(tx_status), "Building & Broadcasting Tx (Sweep to self)...");
                    startFrame(); clearScreen(0xFF1E1E1E); intraFontPrintf(font, 20, 245, "%s", tx_status); endFrame();

                    char txid[70];
                    int ret = send_bitcoin(&global_node, btc_address, btc_address, 0, txid, sizeof(txid));
                    if (ret == 0) {
                        snprintf(tx_status, sizeof(tx_status), "Sent! TXID: %.15s...", txid);
                    } else if (ret == -5) {
                        snprintf(tx_status, sizeof(tx_status), "Balance too low for 500 sat fee");
                    } else {
                        snprintf(tx_status, sizeof(tx_status), "Error broadcasting (%d)", ret);
                    }
                } else {
                    snprintf(tx_status, sizeof(tx_status), "WiFi failed");
                }
            }
        }
        
        sceKernelDelayThread(10000); // 10ms delay to avoid CPU hogging
    }

    if (font) intraFontUnload(font);
    sceKernelExitGame();
    return 0;
}
