#ifndef NETWORK_H
#define NETWORK_H

/*
 * PSP WiFi bring-up, modelled on CMFileManager-PSP (joel16), which connects
 * reliably on 6.61 CFW.
 *
 * The key decision: we do NOT call sceNetApctlConnect() ourselves. The firmware
 * netconf dialog (PSP_NETCONF_ACTION_CONNECTAP) performs the connection, which
 * is the same path the XMB and the browser use. Driving apctl by hand requires
 * reading the saved profile through sceUtility*NetParam, and those calls return
 * 0x8002013A (LIBRARY_NOT_YET_LINKED) outside the XMB - the netparam backend is
 * not resident for homebrew - so apctl accepted the request and then failed
 * asynchronously with 0x80410208.
 *
 * Usage:
 *   1. net_wlan_switch_on()   -> hardware WLAN slider must be ON
 *   2. init_networking()      -> modules + net/inet/resolver/apctl (once)
 *   3. net_connect_dialog()   -> firmware picker; blocks until dismissed
 */

/* 1 when the physical WLAN slider is on. Nothing else works when it is off. */
int net_wlan_switch_on(void);

/* Loads net modules and initialises net/inet/resolver/apctl. 0 on success. */
int init_networking(void);

/* Where the last init_networking()/net_connect_dialog() call got to. */
const char *net_init_detail(void);

/*
 * Runs the firmware connection dialog and returns 0 once an IP is assigned.
 * Renders its own background frames, so the caller must not be mid-frame.
 */
int net_connect_dialog(void);

/* Current PSP_NET_APCTL_STATE_* value, or a negative error. */
int net_state(void);

/* 1 once the AP handed out an IP address. */
int net_is_connected(void);

/* Last error reported by the apctl event handler (0 when none). */
int net_last_error(void);

/* Human readable name for a PSP_NET_APCTL_STATE_* value. */
const char *net_state_name(int state);

/* Copies the assigned IP into buf. 0 on success. */
int net_get_ip(char *buf, int len);

/*
 * Compact trace of apctl events as "e<event>s<newState>[!<error>]" tokens.
 * Where the chain stops identifies a failure, so this is the main diagnostic.
 */
int net_event_trace(char *buf, int len);

/* SSID / security type / channel / signal apctl currently reports. */
int net_ap_info(char *buf, int len);

void terminate_networking(void);

#endif
