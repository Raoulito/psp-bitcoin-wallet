#include <pspkernel.h>
#include <pspsysmem.h>
#include <psppower.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_modules.h>
#include <psputility_netconf.h>
#include <psputility_sysparam.h>
#include <string.h>
#include <stdio.h>

#include "network.h"
#include "graphics.h"

static int  g_initialized = 0;
static int  g_handler_id = -1;
static int  g_apctl_error = 0;
static char g_init_detail[96] = "";

/*
 * apctl runs its own thread and reports failures through this callback only.
 * Every event is recorded, not just errors: the point in the chain where the
 * handshake dies is what identifies a cause (never leaving SCANNING = AP not
 * found; dying at KEY_EXCHANGE = crypto; dying at GETTING_IP = DHCP).
 */
#define APCTL_TRACE_MAX 10

static struct {
    short ev;
    short ns;
    int   err;
} g_ev[APCTL_TRACE_MAX];
static int g_ev_n = 0;

static void apctl_handler(int oldState, int newState, int event, int error, void *pArg)
{
    (void)oldState;
    (void)pArg;

    if (g_ev_n < APCTL_TRACE_MAX) {
        g_ev[g_ev_n].ev  = (short)event;
        g_ev[g_ev_n].ns  = (short)newState;
        g_ev[g_ev_n].err = error;
        g_ev_n++;
    }

    if (event == PSP_NET_APCTL_EVENT_ERROR && error != 0) {
        g_apctl_error = error;
    }
    if (newState == PSP_NET_APCTL_STATE_GOT_IP) {
        g_apctl_error = 0;
    }
}

int net_wlan_switch_on(void)
{
    return sceWlanGetSwitchState() == 1;
}

/*
 * 0 = not loaded, 1 = loaded via sceUtilityLoadModule (fw 2.0+ API),
 * 2 = loaded via sceUtilityLoadNetModule (1.5-era API).
 */
static int g_module_api = 0;

static int module_ok(int rc, unsigned already)
{
    return (rc >= 0) || ((unsigned)rc == already);
}

/*
 * sceUtilityLoadNetModule() is the 1.5-era call. On 6.61 it brings up enough for
 * sockets, but sceUtilityNetconfInitStart() and the sceUtility*NetParam calls
 * still fail with 0x8002013A (LIBRARY_NOT_YET_LINKED), so prefer the 2.0+
 * sceUtilityLoadModule() API and only fall back if it is unavailable.
 */
static int load_net_modules(int free_kb)
{
    int rc_common;
    int rc_inet;

    rc_common = sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    rc_inet   = sceUtilityLoadModule(PSP_MODULE_NET_INET);

    if (module_ok(rc_common, SCE_ERROR_MODULE_ALREADY_LOADED) &&
        module_ok(rc_inet, SCE_ERROR_MODULE_ALREADY_LOADED)) {
        g_module_api = 1;
        snprintf(g_init_detail, sizeof(g_init_detail), "mod2 ok free %dKB", free_kb);
        return 0;
    }

    rc_common = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    rc_inet   = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

    if (module_ok(rc_common, SCE_ERROR_NET_MODULE_NOT_LOADED) &&
        module_ok(rc_inet, SCE_ERROR_NET_MODULE_NOT_LOADED)) {
        g_module_api = 2;
        snprintf(g_init_detail, sizeof(g_init_detail), "mod1 ok free %dKB", free_kb);
        return 0;
    }

    snprintf(g_init_detail, sizeof(g_init_detail), "mod c=%08X i=%08X free %dKB",
             (unsigned)rc_common, (unsigned)rc_inet, free_kb);
    return rc_common < 0 ? rc_common : rc_inet;
}

int init_networking(void)
{
    int rc;
    int free_kb;

    if (g_initialized) return 0;

    /* Net module loads fail when PSP_HEAP_SIZE_KB has claimed most of the user
       partition, so record the headroom before touching anything. */
    free_kb = (int)(sceKernelMaxFreeMemSize() / 1024);
    snprintf(g_init_detail, sizeof(g_init_detail), "free %dKB", free_kb);

    /* Keep the console awake while the radio is up. */
    scePowerLock(0);

    rc = load_net_modules(free_kb);
    if (rc < 0) goto fail;

    /*
     * Initialised by hand rather than with pspSdkInetInit(), which uses
     * sceNetApctlInit(0x1600, 0x42) - a 5.6KB apctl thread stack. These are the
     * values CMFileManager-PSP uses, including a 32KB apctl stack.
     */
    rc = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (rc < 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "sceNetInit 0x%08X", (unsigned)rc);
        goto fail;
    }
    rc = sceNetInetInit();
    if (rc < 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "InetInit 0x%08X", (unsigned)rc);
        goto fail;
    }
    /* CMFileManager skips this (FTP needs no DNS); we need it for mempool.space. */
    rc = sceNetResolverInit();
    if (rc < 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "ResolverInit 0x%08X", (unsigned)rc);
        goto fail;
    }
    rc = sceNetApctlInit(0x8000, 48);
    if (rc < 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "ApctlInit 0x%08X", (unsigned)rc);
        goto fail;
    }

    g_handler_id = sceNetApctlAddHandler(apctl_handler, NULL);

    snprintf(g_init_detail, sizeof(g_init_detail), "ok mod%d free %dKB",
             g_module_api, (int)(sceKernelMaxFreeMemSize() / 1024));
    g_initialized = 1;
    return 0;

fail:
    scePowerUnlock(0);
    return rc;
}

const char *net_init_detail(void)
{
    return g_init_detail;
}

int net_connect_dialog(void)
{
    pspUtilityNetconfData data;
    struct pspUtilityNetconfAdhoc adhocparam;
    int lang = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    int swap = PSP_UTILITY_ACCEPT_CROSS;
    int seen_visible = 0;
    int frames = 0;
    int done = 0;
    int rc;

    if (!g_initialized) return -1;

    g_apctl_error = 0;
    g_ev_n = 0;

    /* Match the user's system settings so the dialog behaves like the XMB. */
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &lang);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &swap);

    memset(&data, 0, sizeof(data));
    memset(&adhocparam, 0, sizeof(adhocparam));

    data.base.size           = sizeof(data);
    data.base.language       = lang;
    data.base.buttonSwap     = swap;
    data.base.graphicsThread = 17;
    data.base.accessThread   = 19;
    data.base.fontThread     = 18;
    data.base.soundThread    = 16;
    data.action              = PSP_NETCONF_ACTION_CONNECTAP;
    data.hotspot             = 0;
    data.adhocparam          = &adhocparam;

    rc = sceUtilityNetconfInitStart(&data);
    if (rc != 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "netconf 0x%08X (mod%d)",
                 (unsigned)rc, g_module_api);
        return rc;
    }

    /* ~60s cap so a dialog that never appears cannot hang the wallet. */
    while (!done && frames < 3600) {
        /*
         * The dialog draws straight into the framebuffer, so the GU list must be
         * finished and synced BEFORE sceUtilityNetconfUpdate(), and the buffer
         * swap must happen after it. Same ordering as CMFileManager.
         */
        startFrame();
        clearScreen(0xFF1E1E1E);
        endFrame_noSwap();

        switch (sceUtilityNetconfGetStatus()) {
            case PSP_UTILITY_DIALOG_NONE:
                /* NONE also occurs briefly before the dialog appears, so only
                   treat it as finished once it has actually been visible. */
                if (seen_visible) done = 1;
                break;

            case PSP_UTILITY_DIALOG_INIT:
                break;

            case PSP_UTILITY_DIALOG_VISIBLE:
                seen_visible = 1;
                if (sceUtilityNetconfUpdate(1) < 0) done = 1;
                break;

            case PSP_UTILITY_DIALOG_QUIT:
                sceUtilityNetconfShutdownStart();
                break;

            case PSP_UTILITY_DIALOG_FINISHED:
                done = 1;
                break;

            default:
                break;
        }

        swapBuffers();
        frames++;
    }

    if (!seen_visible) {
        snprintf(g_init_detail, sizeof(g_init_detail), "dialog never appeared");
        return -2;
    }

    return net_is_connected() ? 0 : -1;
}

int net_state(void)
{
    int state = 0;
    int rc;

    /* Never touch apctl before the net PRXs are resident: the import stubs are
       only patched once the modules load. */
    if (!g_initialized) return -1;

    rc = sceNetApctlGetState(&state);
    if (rc != 0) return rc;
    return state;
}

int net_is_connected(void)
{
    return net_state() == PSP_NET_APCTL_STATE_GOT_IP;
}

int net_last_error(void)
{
    return g_apctl_error;
}

const char *net_state_name(int state)
{
    switch (state) {
        case PSP_NET_APCTL_STATE_DISCONNECTED: return "Disconnected";
        case PSP_NET_APCTL_STATE_SCANNING:     return "Scanning";
        case PSP_NET_APCTL_STATE_JOINING:      return "Joining AP";
        case PSP_NET_APCTL_STATE_GETTING_IP:   return "Getting IP";
        case PSP_NET_APCTL_STATE_GOT_IP:       return "Connected";
        case PSP_NET_APCTL_STATE_EAP_AUTH:     return "EAP auth";
        case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "Key exchange";
        default:                               return "Unknown";
    }
}

int net_get_ip(char *buf, int len)
{
    union SceNetApctlInfo info;

    if (!buf || len <= 0) return -1;
    buf[0] = '\0';
    if (!g_initialized) return -1;

    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) != 0) return -1;

    strncpy(buf, info.ip, len - 1);
    buf[len - 1] = '\0';
    return 0;
}

int net_event_trace(char *buf, int len)
{
    int i;
    int used = 0;

    if (!buf || len <= 0) return -1;
    buf[0] = '\0';

    for (i = 0; i < g_ev_n; i++) {
        char part[32];
        int n;
        if (g_ev[i].err) {
            n = snprintf(part, sizeof(part), "e%ds%d!%08X ",
                         g_ev[i].ev, g_ev[i].ns, (unsigned)g_ev[i].err);
        } else {
            n = snprintf(part, sizeof(part), "e%ds%d ", g_ev[i].ev, g_ev[i].ns);
        }
        if (n < 0 || used + n >= len) break;
        memcpy(buf + used, part, (size_t)n + 1);
        used += n;
    }
    return 0;
}

int net_ap_info(char *buf, int len)
{
    union SceNetApctlInfo info;
    char ssid[33] = "?";
    unsigned sec = 0;
    unsigned strength = 0;
    unsigned chan = 0;

    if (!buf || len <= 0) return -1;
    buf[0] = '\0';
    if (!g_initialized) return -1;

    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info) == 0) {
        memcpy(ssid, info.ssid, 32);
        ssid[32] = '\0';
    }
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SECURITY_TYPE, &info) == 0) {
        sec = info.securityType;
    }
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_STRENGTH, &info) == 0) {
        strength = info.strength;
    }
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_CHANNEL, &info) == 0) {
        chan = info.channel;
    }

    snprintf(buf, len, "ssid=%s sec=%u ch=%u sig=%u", ssid, sec, chan, strength);
    return 0;
}

void terminate_networking(void)
{
    if (!g_initialized) return;

    if (g_handler_id >= 0) {
        sceNetApctlDelHandler(g_handler_id);
        g_handler_id = -1;
    }

    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    if (g_module_api == 1) {
        sceUtilityUnloadModule(PSP_MODULE_NET_INET);
        sceUtilityUnloadModule(PSP_MODULE_NET_COMMON);
    } else if (g_module_api == 2) {
        sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }
    g_module_api = 0;
    scePowerUnlock(0);

    g_initialized = 0;
}
