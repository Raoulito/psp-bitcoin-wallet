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
    unsigned heartbeat;   /* incremented every rendered frame */
    unsigned pad;         /* raw button mask last read */
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

        /*
         * Heartbeat. If this number is moving the main loop is alive and any
         * unresponsiveness is in input handling; if it is frozen the loop is
         * blocked somewhere. W.pad is the raw button mask, so a stuck value of
         * 0 while pressing keys means the pad is not being sampled.
         */
        intraFontSetStyle(font, 0.6f, 0xFF666666, 0, 0, INTRAFONT_ALIGN_RIGHT);
        intraFontPrintf(font, 470, 270, "%u %04X", W.heartbeat, W.pad);
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
 * Brings WiFi up the way pspsdk's samples/net/simple does, which is verified
 * working on this console: connect to the saved config slot, then poll state
 * until GOT_IP.
 *
 * CRITICAL: apctl EVENT_ERROR must not abort this. A WPA association passes
 * through KEY_EXCHANGE (state 6) and raises transient errors such as
 * 0x80410208 before recovering. Earlier versions bailed out there and killed
 * connections that were succeeding. Errors are logged, never acted on.
 */
#define NET_CONFIG_SLOT 1
#define CONNECT_TICKS   90    /* 90 * 500ms = 45s */

static int wifi_connect_ui(char *status, size_t status_len)
{
    char line[68];
    char ip[24] = "?";
    int rc;
    int t;
    int last = -1;

    if (net_is_connected()) return 0;

    net_log_reset();

    if (!net_wlan_switch_on()) {
        snprintf(status, status_len, "WLAN switch is OFF - slide it on, then retry");
        draw_full_ui();
        return -1;
    }

    /*
     * Driven one step at a time, drawing the name of each call BEFORE making it.
     * These calls can block, and running them back to back left no way to tell
     * which one had wedged. Whatever is on screen names the offending call.
     */
    for (t = 0; t <= 6; t++) {
        snprintf(status, status_len, "init: %s", net_init_step_name(t));
        draw_full_ui();

        rc = net_init_step(t);
        if (rc < 0) {
            net_log_add("%s", net_init_detail());
            snprintf(status, status_len, "Net init failed - see net log");
            draw_full_ui();
            return -1;
        }
        if (rc == 1) break;
    }

    snprintf(status, status_len, "Connecting to WiFi...");
    draw_full_ui();

    rc = net_connect(NET_CONFIG_SLOT);
    if (rc != 0) {
        net_log_add("init: %s", net_init_detail());
        net_log_add("apctlConnect=0x%08X", (unsigned)rc);
        snprintf(status, status_len, "Connect failed - see net log");
        draw_full_ui();
        return -1;
    }

    for (t = 0; t < CONNECT_TICKS; t++) {
        int state = net_state();

        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            net_get_ip(ip, sizeof(ip));
            net_log_reset();               /* success: restore mnemonic view */
            snprintf(status, status_len, "WiFi connected (%s)", ip);
            draw_full_ui();
            return 0;
        }

        if (state != last) {
            last = state;
            snprintf(status, status_len, "%s... %ds",
                     net_state_name(state), (CONNECT_TICKS - t) / 2);
            draw_full_ui();
        }

        sceKernelDelayThread(500 * 1000);
    }

    net_log_add("init: %s", net_init_detail());
    net_log_add("timeout in state %d (%s)", last, net_state_name(last));
    if (net_event_trace(line, sizeof(line)) == 0 && line[0]) net_log_add("%s", line);
    if (net_ap_info(line, sizeof(line)) == 0 && line[0]) net_log_add("%s", line);

    snprintf(status, status_len, "WiFi timeout - see net log");
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

    /* The pspsdk samples always set these before reading the pad; without them
       sceCtrlReadBufferPositive() behaviour is not guaranteed. */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    memset(&W, 0, sizeof(W));
    strcpy(W.mnemonic, "Press X to generate mnemonic");
    strcpy(W.balance_str, "Balance: Press [SQUARE] to check");

    load_seed_from_ms();

    while (1) {
        draw_full_ui();

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);

        W.heartbeat++;
        W.pad = pad.Buttons;

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

                /* PBKDF2 + 5 EC derivations take seconds on a PSP; show
                   something first so it does not look like a freeze. */
                net_log_reset();
                snprintf(W.balance_str, sizeof(W.balance_str), "Deriving keys...");
                draw_full_ui();

                derive_wallet();
                snprintf(W.balance_str, sizeof(W.balance_str), "Balance: Press [SQUARE] to check");
                W.tx_status[0] = '\0';
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
                        int tls = http_last_tls_error();
                        net_log_reset();
                        net_log_add("%s (%d)", http_strerror(ret), ret);
                        if (tls != 0) {
                            int i;
                            net_log_add("mbedtls -0x%04X free %dKB",
                                        (unsigned)(-tls), http_last_tls_free_kb());
                            /* Tail of the handshake trace: the last line before
                               the failure names the site that raised it. */
                            for (i = 0; i < http_tls_debug_count(); i++) {
                                net_log_add("%s", http_tls_debug_line(i));
                            }
                        }
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
