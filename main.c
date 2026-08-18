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
#include <fcntl.h>
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
    int is_testnet = 0;
    
    // Attempt to load mnemonic from Memory Stick
    SceUID fd = sceIoOpen("ms0:/psp-wallet-seed.txt", PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        memset(mnemonic, 0, sizeof(mnemonic));
        sceIoRead(fd, mnemonic, sizeof(mnemonic)-1);
        sceIoClose(fd);
        
        // Strip newlines if any
        char *newline = strchr(mnemonic, '\n');
        if (newline) *newline = '\0';
        newline = strchr(mnemonic, '\r');
        if (newline) *newline = '\0';
        
        if (strlen(mnemonic) > 10) {
            uint8_t seed[64];
            mnemonic_to_seed(mnemonic, "", seed, 0);
            hdnode_from_seed(seed, 64, SECP256K1_NAME, &global_node);
            
            hdnode_private_ckd(&global_node, 44 | 0x80000000);
            hdnode_private_ckd(&global_node, (is_testnet ? 1 : 0) | 0x80000000);
            hdnode_private_ckd(&global_node, 0 | 0x80000000);
            hdnode_private_ckd(&global_node, 0);
            hdnode_private_ckd(&global_node, 0);
            hdnode_fill_public_key(&global_node);
            has_node = 1;
            
            ecdsa_get_address(global_node.public_key, is_testnet ? 0x6F : 0x00, HASHER_SHA2_RIPEMD, HASHER_SHA2D, btc_address, sizeof(btc_address));
        }
    }

void draw_full_ui(const char* mnemonic, const char* btc_address, const char* balance_str, const char* tx_status, int is_testnet) {
    startFrame();
    clearScreen(0xFF1E1E1E);

    if (font) {
        intraFontSetStyle(font, 1.0f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 40, "PSP Bitcoin Wallet");

        intraFontSetStyle(font, 0.7f, 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 80, "Mnemonic:");
        
        intraFontSetStyle(font, 0.6f, 0xFFDDDDDD, 0, 0, INTRAFONT_ALIGN_LEFT);
        char formatted_mnemonic[256] = "";
        if (strlen(mnemonic) > 10 && strncmp(mnemonic, "Press", 5) != 0) {
            int spaces = 0;
            int j = 0;
            for (int i = 0; mnemonic[i] != '\0'; i++) {
                if (mnemonic[i] == ' ') {
                    spaces++;
                    if (spaces == 4 || spaces == 8) {
                        formatted_mnemonic[j++] = '\n';
                        continue;
                    }
                }
                formatted_mnemonic[j++] = mnemonic[i];
            }
            formatted_mnemonic[j] = '\0';
        } else {
            strcpy(formatted_mnemonic, mnemonic);
        }
        intraFontPrintf(font, 20, 110, "%s", formatted_mnemonic);

        intraFontSetStyle(font, 0.7f, is_testnet ? 0xFF00AAFF : 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 160, "Address (%s):", is_testnet ? "TESTNET" : "MAINNET");
        
        intraFontSetStyle(font, 0.8f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 190, "%s", btc_address);

        intraFontSetStyle(font, 0.7f, 0xFF00FFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 220, "%s", balance_str);

        intraFontSetStyle(font, 0.7f, 0xFFFFAA00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 245, "%s", tx_status);

        intraFontSetStyle(font, 0.6f, 0xFFAAAAAA, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 255, "[X] Gen   [SQUARE] Bal   [TRIANGLE] Sweep");
        intraFontPrintf(font, 20, 270, "[L/R] Toggle Testnet     [START] Exit");
    }
    endFrame();
}

    while(1) {
        draw_full_ui(mnemonic, btc_address, balance_str, tx_status, is_testnet);

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        
        // Edge detection for buttons
        int btnDown = pad.Buttons & ~oldButtons;
        oldButtons = pad.Buttons;
        
        if (btnDown & PSP_CTRL_START) {
            break;
        }
        if (btnDown & PSP_CTRL_CROSS) {
            uint8_t data[16];
            random_buffer(data, 16);
            const char* generated = mnemonic_from_data(data, 16);
            if (generated) {
                strncpy(mnemonic, generated, sizeof(mnemonic)-1);
                
                // Save to ms0:/psp-wallet-seed.txt
                SceUID fd = sceIoOpen("ms0:/psp-wallet-seed.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
                if (fd >= 0) {
                    sceIoWrite(fd, mnemonic, strlen(mnemonic));
                    sceIoClose(fd);
                }

                uint8_t seed[64];
                mnemonic_to_seed(mnemonic, "", seed, 0);

                hdnode_from_seed(seed, 64, SECP256K1_NAME, &global_node);

                hdnode_private_ckd(&global_node, 44 | 0x80000000);
                hdnode_private_ckd(&global_node, (is_testnet ? 1 : 0) | 0x80000000);
                hdnode_private_ckd(&global_node, 0 | 0x80000000);
                hdnode_private_ckd(&global_node, 0);
                hdnode_private_ckd(&global_node, 0);
                hdnode_fill_public_key(&global_node);
                has_node = 1;

                ecdsa_get_address(global_node.public_key, is_testnet ? 0x6F : 0x00, HASHER_SHA2_RIPEMD, HASHER_SHA2D, btc_address, sizeof(btc_address));
                snprintf(balance_str, sizeof(balance_str), "Balance: Press [SQUARE] to check");
                tx_status[0] = '\0';
            }
        }
        
        if (btnDown & PSP_CTRL_LTRIGGER || btnDown & PSP_CTRL_RTRIGGER) {
            is_testnet = !is_testnet;
            if (has_node) {
                // Re-derive address for the new network
                uint8_t seed[64];
                mnemonic_to_seed(mnemonic, "", seed, 0);
                hdnode_from_seed(seed, 64, SECP256K1_NAME, &global_node);
                hdnode_private_ckd(&global_node, 44 | 0x80000000);
                hdnode_private_ckd(&global_node, (is_testnet ? 1 : 0) | 0x80000000);
                hdnode_private_ckd(&global_node, 0 | 0x80000000);
                hdnode_private_ckd(&global_node, 0);
                hdnode_private_ckd(&global_node, 0);
                hdnode_fill_public_key(&global_node);
                
                ecdsa_get_address(global_node.public_key, is_testnet ? 0x6F : 0x00, HASHER_SHA2_RIPEMD, HASHER_SHA2D, btc_address, sizeof(btc_address));
                snprintf(balance_str, sizeof(balance_str), "Balance: Press [SQUARE] to check");
                tx_status[0] = '\0';
            }
        }

        if (btnDown & PSP_CTRL_SQUARE) {
            if (strlen(btc_address) > 0) {
                if (!wifi_connected) {
                    snprintf(balance_str, sizeof(balance_str), "Connecting to WiFi (Slot 1)...");
                    draw_full_ui(mnemonic, btc_address, balance_str, tx_status, is_testnet);

                    int init_ret = init_networking();
                    if (init_ret == 0) {
                        int conn_ret = connect_to_ap();
                        if (conn_ret == 0) {
                            wifi_connected = 1;
                        } else {
                            snprintf(balance_str, sizeof(balance_str), "Error: AP Connect Failed (%d)", conn_ret);
                        }
                    } else {
                        snprintf(balance_str, sizeof(balance_str), "Error: Net Init Failed (%d)", init_ret);
                    }
                }

                if (wifi_connected) {
                    snprintf(balance_str, sizeof(balance_str), "Fetching balance from mempool.space...");
                    draw_full_ui(mnemonic, btc_address, balance_str, tx_status, is_testnet);

                    uint64_t sats = 0;
                    int ret = fetch_bitcoin_balance(btc_address, is_testnet, &sats);
                    if (ret == 0) {
                        snprintf(balance_str, sizeof(balance_str), "Balance: %llu satoshis", (unsigned long long)sats);
                    } else {
                        snprintf(balance_str, sizeof(balance_str), "Error: HTTP fetch failed (%d)", ret);
                    }
                    }
                }
        }

        if (btnDown & PSP_CTRL_TRIANGLE) {
            if (has_node && strlen(btc_address) > 0) {
                if (!wifi_connected) {
                    snprintf(tx_status, sizeof(tx_status), "Connecting to WiFi (Slot 1)...");
                    draw_full_ui(mnemonic, btc_address, balance_str, tx_status, is_testnet);
                    
                    int init_ret = init_networking();
                    if (init_ret == 0) {
                        int conn_ret = connect_to_ap();
                        if (conn_ret == 0) {
                            wifi_connected = 1;
                        } else {
                            snprintf(tx_status, sizeof(tx_status), "Error: AP Connect Failed (%d)", conn_ret);
                        }
                    } else {
                        snprintf(tx_status, sizeof(tx_status), "Error: Net Init Failed (%d)", init_ret);
                    }
                }

                if (wifi_connected) {
                    snprintf(tx_status, sizeof(tx_status), "Building & Broadcasting Tx (Sweep to self)...");
                    draw_full_ui(mnemonic, btc_address, balance_str, tx_status, is_testnet);

                    char txid[70];
                    int ret = send_bitcoin(&global_node, btc_address, btc_address, 0, is_testnet, txid, sizeof(txid));
                    if (ret == 0) {
                        snprintf(tx_status, sizeof(tx_status), "Sent! TXID: %.15s...", txid);
                    } else if (ret == -5) {
                        snprintf(tx_status, sizeof(tx_status), "Balance too low for 500 sat fee");
                    } else {
                        snprintf(tx_status, sizeof(tx_status), "Error broadcasting (%d)", ret);
                    }
                    }
                }
        }
        
        sceKernelDelayThread(10000); // 10ms delay to avoid CPU hogging
    }

    if (font) intraFontUnload(font);
    sceKernelExitGame();
    return 0;
}
