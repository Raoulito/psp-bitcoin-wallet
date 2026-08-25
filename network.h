#ifndef NETWORK_H
#define NETWORK_H

/*
 * PSP WiFi bring-up, matching pspsdk's samples/net/simple, which is verified
 * working on this console (states 0 -> 2 -> 6 -> 4, IP assigned).
 *
 *   sceUtilityLoadNetModule(COMMON) + (INET)
 *   pspSdkInetInit()
 *   sceNetApctlConnect(config)
 *   poll sceNetApctlGetState() until PSP_NET_APCTL_STATE_GOT_IP
 *
 * Two things this deliberately does NOT do, both of which broke it before:
 *
 * 1. It does not treat apctl EVENT_ERROR as fatal. A WPA association passes
 *    through KEY_EXCHANGE (state 6) and emits transient errors there, such as
 *    0x80410208, then recovers. The sample registers no handler at all and
 *    simply waits for state 4. Errors here are recorded for diagnostics only.
 *
 * 2. It does not use the netconf dialog. sceUtilityNetconfInitStart() returns
 *    0x8002013A (LIBRARY_NOT_YET_LINKED) on this firmware even at kernel
 *    privilege level, so that dialog is unavailable and is not used.
 */

/* 1 when the physical WLAN slider is on. Nothing else works when it is off. */
int net_wlan_switch_on(void);

/* Loads net modules and initialises net/inet/resolver/apctl. 0 on success. */
int init_networking(void);

/*
 * Step-wise variant of init_networking().
 *
 * Each of these calls can block, and previously they ran back to back with no
 * chance to redraw, so a hang was indistinguishable between them. The caller
 * draws net_init_step_name(step) *before* invoking net_init_step(step), which
 * means whatever is on screen when it wedges names the offending call.
 *
 * Returns 0 to continue, 1 when initialisation is complete, < 0 on error.
 */
const char *net_init_step_name(int step);
int net_init_step(int step);

/* Where the last init_networking() call got to, for on-screen diagnostics. */
const char *net_init_detail(void);

/* Starts an async connection to a saved config slot (1-based). 0 on success. */
int net_connect(int config_id);

/* Current PSP_NET_APCTL_STATE_* value, or a negative error. */
int net_state(void);

/* 1 once the AP handed out an IP address. */
int net_is_connected(void);

/* Last apctl error seen. Informational only - never a reason to give up. */
int net_last_error(void);

/* Human readable name for a PSP_NET_APCTL_STATE_* value. */
const char *net_state_name(int state);

/* Copies the assigned IP into buf. 0 on success. */
int net_get_ip(char *buf, int len);

/* Compact apctl event trace: "e<event>s<newState>[!<error>]" tokens. */
int net_event_trace(char *buf, int len);

/* SSID / security type / channel / signal apctl currently reports. */
int net_ap_info(char *buf, int len);

void terminate_networking(void);

#endif
