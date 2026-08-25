#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspsdk.h>
#include <string.h>
#include <stdio.h>

#include "network.h"

static int  g_initialized = 0;
static int  g_handler_id = -1;
static int  g_apctl_error = 0;
static char g_init_detail[96] = "";
static int  g_free_kb_at_init = 0;

/*
 * Diagnostics only.
 *
 * The handler exists so a failure can be described after the fact; it must never
 * be used to abort a connection. apctl raises EVENT_ERROR during a normal WPA
 * key exchange and then recovers - acting on it aborted associations that were
 * about to succeed. pspsdk's working sample registers no handler whatsoever.
 */
#define APCTL_TRACE_MAX 12

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

const char *net_init_step_name(int step)
{
    switch (step) {
        case 0: return "sceUtilityLoadNetModule(COMMON)";
        case 1: return "sceUtilityLoadNetModule(INET)";
        case 2: return "sceNetInit";
        case 3: return "sceNetInetInit";
        case 4: return "sceNetResolverInit";
        case 5: return "sceNetApctlInit";
        case 6: return "sceNetApctlAddHandler";
        default: return "done";
    }
}

int net_init_step(int step)
{
    int rc = 0;

    if (g_initialized) return 1;

    switch (step) {
        case 0:
            g_free_kb_at_init = (int)(sceKernelMaxFreeMemSize() / 1024);
            rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
            break;
        case 1:
            rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
            break;
        case 2:
            /* Same parameters pspSdkInetInit() and the working sample use. */
            rc = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
            break;
        case 3:
            rc = sceNetInetInit();
            break;
        case 4:
            /* Needed for DNS; the FTP-only reference app skips this. */
            rc = sceNetResolverInit();
            break;
        case 5:
            rc = sceNetApctlInit(0x8000, 48);
            break;
        case 6:
            g_handler_id = sceNetApctlAddHandler(apctl_handler, NULL);
            snprintf(g_init_detail, sizeof(g_init_detail), "ok free %dKB",
                     g_free_kb_at_init);
            g_initialized = 1;
            return 1;
        default:
            return 1;
    }

    if (rc < 0) {
        snprintf(g_init_detail, sizeof(g_init_detail), "%s = 0x%08X free %dKB",
                 net_init_step_name(step), (unsigned)rc, g_free_kb_at_init);
        return rc;
    }
    return 0;
}

int init_networking(void)
{
    int step;
    int rc;

    for (step = 0; step <= 6; step++) {
        rc = net_init_step(step);
        if (rc < 0) return rc;
        if (rc == 1) return 0;
    }
    return 0;
}

const char *net_init_detail(void)
{
    return g_init_detail;
}

int net_connect(int config_id)
{
    int state = 0;
    int i;

    if (!g_initialized) return -1;

    /* Only tear down a genuinely half-finished attempt. Never disconnect while
       an association is in progress. */
    if (sceNetApctlGetState(&state) == 0 &&
        state != PSP_NET_APCTL_STATE_DISCONNECTED &&
        state != PSP_NET_APCTL_STATE_GOT_IP) {
        sceNetApctlDisconnect();
        for (i = 0; i < 30; i++) {
            if (sceNetApctlGetState(&state) != 0) break;
            if (state == PSP_NET_APCTL_STATE_DISCONNECTED) break;
            sceKernelDelayThread(100 * 1000);
        }
    }

    /* Cleared after any teardown, never before: sceNetApctlDisconnect() can
       raise its own error event. */
    g_apctl_error = 0;
    g_ev_n = 0;

    return sceNetApctlConnect(config_id);
}

int net_state(void)
{
    int state = 0;
    int rc;

    /* Never touch apctl before the net PRXs are resident. */
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
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);

    g_initialized = 0;
}
