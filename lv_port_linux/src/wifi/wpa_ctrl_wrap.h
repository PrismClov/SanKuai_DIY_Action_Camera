/**
 * @file wpa_ctrl_wrap.h
 * @brief Thin wrapper around wpa_supplicant control interface (wpa_ctrl).
 *
 * Drives WiFi on RK3566 buildroot via the wpa_supplicant control socket
 * (/var/run/wpa_supplicant/wlan0). Two independent wpa_ctrl connections are
 * used because wpa_ctrl handles are not thread-safe:
 *   - g_status_conn : main (LVGL) thread, STATUS polling only
 *   - g_cmd_conn    : worker thread, SCAN / connect sequence
 *
 * All blocking calls (SCAN_RESULTS, connect) MUST run on the worker thread.
 */
#ifndef WPA_CTRL_WRAP_H
#define WPA_CTRL_WRAP_H

#include <stddef.h>

#define WPA_CTRL_PATH   "/var/run/wpa_supplicant/wlan0"
#define WPA_REPLY_MAX   8192
#define WIFI_NET_MAX    32
#define WIFI_SSID_MAX   64
#define WIFI_BSSID_MAX  32
#define WIFI_PSK_MAX    64

typedef struct {
    char ssid[WIFI_SSID_MAX];      /* "" for hidden networks */
    char bssid[WIFI_BSSID_MAX];    /* "aa:bb:cc:dd:ee:ff" */
    int  signal_dbm;               /* raw dBm, e.g. -67 */
    int  quality;                  /* 0..100, derived */
    int  freq;                     /* MHz */
    int  secured;                  /* 1 if WPA/WPA2/WEP/RSN, 0 if OPEN */
    char flags[32];                /* raw flags string */
} wifi_net_t;

/* Main-thread status connection (open once, long-lived). Retries on failure. */
int  wpa_init(void);
void wpa_deinit(void);

/* Worker-thread command connection (open per job, close when done). */
int  wpa_worker_init(void);
void wpa_worker_deinit(void);

/* Generic command helper (worker thread only): wraps wpa_ctrl_request on
 * g_cmd_conn, null-terminates and strips trailing CR/LF. 0 on success. */
int  wpa_cmd(const char *cmd, char *reply, size_t reply_len);

/* Scan: SCAN starts async; wpa_scan_results polls SCAN_RESULTS until ready
 * (retries up to ~10s). Returns 0 and fills nets/count on success. */
int  wpa_scan_start(void);
int  wpa_scan_results(wifi_net_t *nets, int max, int *count_out);

/* Connect: ADD_NETWORK + SET_NETWORK ssid/psk(or key_mgmt NONE) + ENABLE +
 * SAVE_CONFIG. Returns network id (>=0) on success, -1 on failure. */
int  wpa_connect(const char *ssid, const char *psk);
int  wpa_disconnect(void);

/* Status (main thread, uses g_status_conn). Parses wpa_state/ssid/ip_address. */
int  wpa_status(char *ssid, size_t ssid_sz,
                char *ip, size_t ip_sz,
                char *state, size_t state_sz);

/* dBm -> 0..100 quality. */
int  wifi_dbm_to_quality(int dbm);

#endif /* WPA_CTRL_WRAP_H */
