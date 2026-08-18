#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psputility.h>
#include <pspnet_apctl.h>
#include <fcntl.h>
#include <stdarg.h>
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

/*
 * CRITICAL: do not raise this back up.
 *
 * A positive PSP_HEAP_SIZE_KB reserves *exactly* that many KB from the ~24MB
 * user partition at startup, before main() runs. The previous value (20480)
 * left barely 1MB free, so sceUtilityLoadNetModule() had nowhere to load
 * pspnet/pspnet_inet/pspnet_apctl/pspnet_resolver into. WiFi therefore failed
 * only inside this app while working everywhere else on the same console, and
 * calling sceNetInit() on the non-resident modules is what produced the
 * "5-second kernel panic" seen earlier.
 *
 * This app's actual heap use is tiny: intraFont's glyph cache, one mbedTLS SSL
 * context (~40KB) and a few-KB hex buffer in tx_builder. 8MB is already ~8x
 * headroom and leaves ~12MB for the network stack.
 */
PSP_HEAP_SIZE_KB(8192);

#define SEED_FILE "ms0:/psp-wallet-seed.txt"

void psp_rand_init(void);

static intraFont *font;

/* UI state lives at file scope so the WiFi helper can redraw while polling. */
static struct {
    char mnemonic[256];
    char btc_address[40];
    char balance_str[128];
    char tx_status[128];
    int  is_testnet;
    HDNode node;
    int  has_node;
    /* Persistent WiFi diagnostics. Each connection attempt appends one line;
       without this every slot's outcome was overwritten by the next one and
       only the final summary survived on screen. */
    char net_log[6][72];
    int  net_log_n;
} W;

static void net_log_reset(void)
{
    W.net_log_n = 0;
}

static void net_log_add(const char *fmt, ...)
{
    va_list ap;

    if (W.net_log_n >= (int)(sizeof(W.net_log) / sizeof(W.net_log[0]))) return;

    va_start(ap, fmt);
    vsnprintf(W.net_log[W.net_log_n], sizeof(W.net_log[0]), fmt, ap);
    va_end(ap);
    W.net_log_n++;
}

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int CallbackThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static int SetupCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
    return thid;
}

static void draw_full_ui(void)
{
    startFrame();
    clearScreen(0xFF1E1E1E);

    if (font) {
        intraFontSetStyle(font, 1.0f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 40, "PSP Bitcoin Wallet");

        intraFontSetStyle(font, 0.7f, 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 80, W.net_log_n ? "Net log:" : "Mnemonic:");

        intraFontSetStyle(font, 0.6f, 0xFFDDDDDD, 0, 0, INTRAFONT_ALIGN_LEFT);
        if (W.net_log_n) {
            for (int i = 0; i < W.net_log_n; i++) {
                intraFontPrintf(font, 20, 96 + i * 10, "%s", W.net_log[i]);
            }
        } else {
            char formatted_mnemonic[288] = "";
            if (strlen(W.mnemonic) > 10 && strncmp(W.mnemonic, "Press", 5) != 0) {
                int spaces = 0;
                int j = 0;
                for (int i = 0; W.mnemonic[i] != '\0'; i++) {
                    if (W.mnemonic[i] == ' ') {
                        spaces++;
                        if (spaces == 4 || spaces == 8) {
                            formatted_mnemonic[j++] = '\n';
                            continue;
                        }
                    }
                    formatted_mnemonic[j++] = W.mnemonic[i];
                }
                formatted_mnemonic[j] = '\0';
            } else {
                strcpy(formatted_mnemonic, W.mnemonic);
            }
            intraFontPrintf(font, 20, 110, "%s", formatted_mnemonic);
        }

        intraFontSetStyle(font, 0.7f, W.is_testnet ? 0xFF00AAFF : 0xFF00FF00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 160, "Address (%s):", W.is_testnet ? "TESTNET" : "MAINNET");

        intraFontSetStyle(font, 0.8f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 190, "%s", W.btc_address);

        intraFontSetStyle(font, 0.7f, 0xFF00FFFF, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 215, "%s", W.balance_str);

        intraFontSetStyle(font, 0.7f, 0xFFFFAA00, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 235, "%s", W.tx_status);

        intraFontSetStyle(font, 0.6f, 0xFFAAAAAA, 0, 0, INTRAFONT_ALIGN_LEFT);
        intraFontPrintf(font, 20, 258, "[X] Gen   [SQUARE] Bal   [TRIANGLE] Sweep");
        intraFontPrintf(font, 20, 270, "[L/R] Toggle Testnet     [START] Exit");
    }
    endFrame();
}

/* BIP44 m/44'/coin'/0'/0/0 for the active network. */
static void derive_wallet(void)
{
    uint8_t seed[64];

    mnemonic_to_seed(W.mnemonic, "", seed, 0);
    hdnode_from_seed(seed, 64, SECP256K1_NAME, &W.node);

    hdnode_private_ckd(&W.node, 44 | 0x80000000);
    hdnode_private_ckd(&W.node, (W.is_testnet ? 1 : 0) | 0x80000000);
    hdnode_private_ckd(&W.node, 0 | 0x80000000);
    hdnode_private_ckd(&W.node, 0);
    hdnode_private_ckd(&W.node, 0);
    hdnode_fill_public_key(&W.node);
    W.has_node = 1;

    ecdsa_get_address(W.node.public_key, W.is_testnet ? 0x6F : 0x00,
                      HASHER_SHA2_RIPEMD, HASHER_SHA2D,
                      W.btc_address, sizeof(W.btc_address));
}

/*
 * Brings WiFi up via the firmware connection dialog and returns 0 once an IP is
 * assigned, otherwise writes the reason into status.
 *
 * We deliberately do not drive sceNetApctlConnect() ourselves - see network.h.
 * Slot-by-slot connection attempts always failed with 0x80410208 because apctl
 * cannot read the saved profile from a homebrew context.
 */
static int wifi_connect_ui(char *status, size_t status_len)
{
    char line[68];
    char ip[24] = "?";
    int rc;

    if (net_is_connected()) return 0;

    net_log_reset();

    if (!net_wlan_switch_on()) {
        snprintf(status, status_len, "WLAN switch is OFF - slide it on, then retry");
        draw_full_ui();
        return -1;
    }

    rc = init_networking();
    if (rc != 0) {
        net_log_add("init failed: %s", net_init_detail());
        snprintf(status, status_len, "Net init failed: %s", net_init_detail());
        draw_full_ui();
        return -1;
    }

    snprintf(status, status_len, "Choose your connection...");
    draw_full_ui();

    /* Hands control to the firmware picker; returns after it is dismissed. */
    rc = net_connect_dialog();

    if (rc == 0) {
        net_get_ip(ip, sizeof(ip));
        net_log_reset();                /* success: restore the mnemonic view */
        snprintf(status, status_len, "WiFi connected (%s)", ip);
        draw_full_ui();
        return 0;
    }

    net_log_add("init: %s", net_init_detail());
    net_log_add("state: %s", net_state_name(net_state()));
    if (net_event_trace(line, sizeof(line)) == 0 && line[0]) net_log_add("%s", line);
    if (net_ap_info(line, sizeof(line)) == 0 && line[0]) net_log_add("%s", line);

    snprintf(status, status_len, "Not connected - see net log");
    draw_full_ui();
    return -1;
}

static void load_seed_from_ms(void)
{
    SceUID fd = sceIoOpen(SEED_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;

    memset(W.mnemonic, 0, sizeof(W.mnemonic));
    sceIoRead(fd, W.mnemonic, sizeof(W.mnemonic) - 1);
    sceIoClose(fd);

    char *nl = strchr(W.mnemonic, '\n');
    if (nl) *nl = '\0';
    nl = strchr(W.mnemonic, '\r');
    if (nl) *nl = '\0';

    if (strlen(W.mnemonic) > 10) derive_wallet();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int oldButtons = 0;

    SetupCallbacks();
    initGraphics();
    psp_rand_init();

    intraFontInit();
    font = intraFontLoad("flash0:/font/ltn0.pgf", 0);

    memset(&W, 0, sizeof(W));
    strcpy(W.mnemonic, "Press X to generate mnemonic");
    strcpy(W.balance_str, "Balance: Press [SQUARE] to check");

    load_seed_from_ms();

    while (1) {
        draw_full_ui();

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);

        int btnDown = pad.Buttons & ~oldButtons;
        oldButtons = pad.Buttons;

        if (btnDown & PSP_CTRL_START) break;

        if (btnDown & PSP_CTRL_CROSS) {
            uint8_t data[16];
            random_buffer(data, 16);
            const char *generated = mnemonic_from_data(data, 16);
            if (generated) {
                strncpy(W.mnemonic, generated, sizeof(W.mnemonic) - 1);
                W.mnemonic[sizeof(W.mnemonic) - 1] = '\0';

                SceUID fd = sceIoOpen(SEED_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
                if (fd >= 0) {
                    sceIoWrite(fd, W.mnemonic, strlen(W.mnemonic));
                    sceIoClose(fd);
                }

                derive_wallet();
                snprintf(W.balance_str, sizeof(W.balance_str), "Balance: Press [SQUARE] to check");
                W.tx_status[0] = '\0';
                net_log_reset();   /* show the mnemonic again */
            }
        }

        if (btnDown & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
            W.is_testnet = !W.is_testnet;
            net_log_reset();
            if (W.has_node) {
                derive_wallet();
                snprintf(W.balance_str, sizeof(W.balance_str), "Balance: Press [SQUARE] to check");
                W.tx_status[0] = '\0';
            }
        }

        if (btnDown & PSP_CTRL_SQUARE) {
            if (strlen(W.btc_address) > 0) {
                if (wifi_connect_ui(W.balance_str, sizeof(W.balance_str)) == 0) {
                    snprintf(W.balance_str, sizeof(W.balance_str), "Fetching from mempool.space...");
                    draw_full_ui();

                    uint64_t sats = 0;
                    int ret = fetch_bitcoin_balance(W.btc_address, W.is_testnet, &sats);
                    if (ret == 0) {
                        snprintf(W.balance_str, sizeof(W.balance_str),
                                 "Balance: %llu sats", (unsigned long long)sats);
                    } else {
                        snprintf(W.balance_str, sizeof(W.balance_str),
                                 "Fetch failed: %s (%d)", http_strerror(ret), ret);
                    }
                }
            }
        }

        if (btnDown & PSP_CTRL_TRIANGLE) {
            if (W.has_node && strlen(W.btc_address) > 0) {
                if (wifi_connect_ui(W.tx_status, sizeof(W.tx_status)) == 0) {
                    snprintf(W.tx_status, sizeof(W.tx_status), "Building & broadcasting sweep...");
                    draw_full_ui();

                    char txid[70];
                    int ret = send_bitcoin(&W.node, W.btc_address, W.btc_address, 0,
                                           W.is_testnet, txid, sizeof(txid));
                    if (ret == 0) {
                        snprintf(W.tx_status, sizeof(W.tx_status), "Sent! TXID: %.15s...", txid);
                    } else if (ret == -5) {
                        snprintf(W.tx_status, sizeof(W.tx_status), "Balance too low for 500 sat fee");
                    } else {
                        snprintf(W.tx_status, sizeof(W.tx_status), "Broadcast error (%d)", ret);
                    }
                }
            }
        }

        sceKernelDelayThread(10000);
    }

    if (font) intraFontUnload(font);
    terminate_networking();
    sceKernelExitGame();
    return 0;
}
