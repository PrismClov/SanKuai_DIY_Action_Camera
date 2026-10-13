/**
 * @file wpa_ctrl_wrap.c
 * @brief wpa_supplicant control interface wrapper implementation.
 */
#include "wpa_ctrl_wrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wpa_ctrl.h>

static struct wpa_ctrl *g_cmd_conn    = NULL; /* worker thread  */
static struct wpa_ctrl *g_status_conn = NULL; /* main (LVGL) thread */

/* ------------------------------------------------------------------ */
/* connection management                                              */
/* ------------------------------------------------------------------ */

int wpa_init(void)
{
    /* App may start before wpa_supplicant; retry a few times. */
    for (int i = 0; i < 10; i++) {
        g_status_conn = wpa_ctrl_open(WPA_CTRL_PATH);
        if (g_status_conn) return 0;
        usleep(500 * 1000);
    }
    g_status_conn = NULL;
    return -1;
}

void wpa_deinit(void)
{
    if (g_status_conn) {
        wpa_ctrl_close(g_status_conn);
        g_status_conn = NULL;
    }
}

int wpa_worker_init(void)
{
    for (int i = 0; i < 5; i++) {
        g_cmd_conn = wpa_ctrl_open(WPA_CTRL_PATH);
        if (g_cmd_conn) return 0;
        usleep(500 * 1000);
    }
    g_cmd_conn = NULL;
    return -1;
}

void wpa_worker_deinit(void)
{
    if (g_cmd_conn) {
        wpa_ctrl_close(g_cmd_conn);
        g_cmd_conn = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* generic command                                                    */
/* ------------------------------------------------------------------ */

int wpa_cmd(const char *cmd, char *reply, size_t reply_len)
{
    if (!g_cmd_conn || !reply || reply_len == 0) return -1;
    reply[0] = '\0';
    size_t n = reply_len - 1;          /* leave room for NUL */
    int r = wpa_ctrl_request(g_cmd_conn, cmd, strlen(cmd), reply, &n, NULL);
    if (r < 0) return -1;
    reply[n] = '\0';
    while (n > 0 && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
        reply[--n] = '\0';
    return 0;
}

/* Internal: send a command on the status connection (main thread). */
static int wpa_cmd_status(const char *cmd, char *reply, size_t reply_len)
{
    if (!g_status_conn || !reply || reply_len == 0) return -1;
    size_t n = reply_len - 1;
    int r = wpa_ctrl_request(g_status_conn, cmd, strlen(cmd), reply, &n, NULL);
    if (r < 0) return -1;
    reply[n] = '\0';
    while (n > 0 && (reply[n - 1] == '\n' || reply[n - 1] == '\r'))
        reply[--n] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

int wifi_dbm_to_quality(int dbm)
{
    if (dbm <= -100) return 0;
    if (dbm >= -50)  return 100;
    return (dbm + 100) * 2;
}

/* Escape \ and " then wrap in double quotes for SET_NETWORK arguments. */
static void wpa_quote(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (out_sz < 3) { if (out_sz > 0) out[0] = '\0'; return; }
    out[o++] = '"';
    for (size_t i = 0; in[i] && o < out_sz - 2; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            if (o < out_sz - 3) out[o++] = '\\';
        }
        out[o++] = in[i];
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* scan                                                               */
/* ------------------------------------------------------------------ */

int wpa_scan_start(void)
{
    char reply[64];
    if (wpa_cmd("SCAN", reply, sizeof(reply)) != 0) return -1;
    /* "OK" => started. "FAIL-BUSY" => scan already running, treat as ok. */
    if (strncmp(reply, "OK", 2) == 0) return 0;
    if (strncmp(reply, "FAIL-BUSY", 9) == 0) return 0;
    return -1;
}

int wpa_scan_results(wifi_net_t *nets, int max, int *count_out)
{
    char buf[WPA_REPLY_MAX] = {0};
    int got = 0;
    int scan_ok = 0;

    if (count_out) *count_out = 0;
    if (!nets || max <= 0) return -1;

    /* Poll until results are ready (scan takes a few seconds). */
    for (int i = 0; i < 40; i++) {            /* up to ~20s */
        buf[0] = '\0';
        if (wpa_cmd("SCAN_RESULTS", buf, sizeof(buf)) != 0) {
            usleep(500 * 1000);
            continue;
        }
        if (strncmp(buf, "FAIL", 4) == 0) {   /* still scanning */
            usleep(500 * 1000);
            continue;
        }
        scan_ok = 1;
        break;
    }
    if (!scan_ok || buf[0] == '\0' || strncmp(buf, "FAIL", 4) == 0) return -1;

    /* Parse: first line is header, then "bssid\tfreq\tsignal\tflags\tssid" */
    char *save_outer = NULL;
    char *line = strtok_r(buf, "\n", &save_outer);
    int line_no = 0;
    for (; line && got < max; line = strtok_r(NULL, "\n", &save_outer), line_no++) {
        if (line_no == 0) continue;           /* header */

        char *save_inner = NULL;
        char *bssid  = strtok_r(line, "\t", &save_inner);
        char *freq   = bssid  ? strtok_r(NULL, "\t", &save_inner) : NULL;
        char *signal = freq   ? strtok_r(NULL, "\t", &save_inner) : NULL;
        char *flags  = signal ? strtok_r(NULL, "\t", &save_inner) : NULL;
        char *ssid   = flags  ? strtok_r(NULL, "\t", &save_inner) : NULL;
        if (!bssid || !freq || !signal || !flags) continue;

        wifi_net_t *n = &nets[got++];
        memset(n, 0, sizeof(*n));
        strncpy(n->bssid, bssid, WIFI_BSSID_MAX - 1);
        n->freq        = atoi(freq);
        n->signal_dbm  = atoi(signal);
        strncpy(n->flags, flags, sizeof(n->flags) - 1);
        n->secured = (strstr(flags, "WPA") || strstr(flags, "WEP") ||
                      strstr(flags, "RSN")) ? 1 : 0;
        n->quality = wifi_dbm_to_quality(n->signal_dbm);
        if (ssid) strncpy(n->ssid, ssid, WIFI_SSID_MAX - 1);
    }

    if (count_out) *count_out = got;
    return 0;
}

/* ------------------------------------------------------------------ */
/* connect                                                            */
/* ------------------------------------------------------------------ */

int wpa_connect(const char *ssid, const char *psk)
{
    char cmd[256];
    char reply[64];
    char esc_ssid[WIFI_SSID_MAX * 2 + 4];
    char esc_psk[WIFI_PSK_MAX * 2 + 4];
    int id;

    if (!ssid || ssid[0] == '\0') return -1;

    /* ADD_NETWORK -> reply is decimal id */
    if (wpa_cmd("ADD_NETWORK", reply, sizeof(reply)) != 0) return -1;
    id = atoi(reply);
    if (id < 0) return -1;

    wpa_quote(ssid, esc_ssid, sizeof(esc_ssid));

    snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid %s", id, esc_ssid);
    if (wpa_cmd(cmd, reply, sizeof(reply)) != 0 || strncmp(reply, "OK", 2) != 0)
        goto fail;

    if (psk && psk[0] != '\0') {
        wpa_quote(psk, esc_psk, sizeof(esc_psk));
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk %s", id, esc_psk);
        if (wpa_cmd(cmd, reply, sizeof(reply)) != 0 || strncmp(reply, "OK", 2) != 0)
            goto fail;
    } else {
        /* Open network */
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
        if (wpa_cmd(cmd, reply, sizeof(reply)) != 0 || strncmp(reply, "OK", 2) != 0)
            goto fail;
    }

    snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", id);
    if (wpa_cmd(cmd, reply, sizeof(reply)) != 0 || strncmp(reply, "OK", 2) != 0)
        goto fail;

    /* Best-effort persist; ignore failure (needs update_config=1). */
    wpa_cmd("SAVE_CONFIG", reply, sizeof(reply));

    return id;

fail:
    snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
    wpa_cmd(cmd, reply, sizeof(reply));
    return -1;
}

int wpa_disconnect(void)
{
    char reply[64];
    int r = wpa_cmd("DISCONNECT", reply, sizeof(reply));
    if (r != 0) return -1;
    return (strncmp(reply, "OK", 2) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* status (main thread)                                               */
/* ------------------------------------------------------------------ */

int wpa_status(char *ssid, size_t ssid_sz,
               char *ip, size_t ip_sz,
               char *state, size_t state_sz)
{
    char buf[WPA_REPLY_MAX];
    if (ssid)    ssid[0] = '\0';
    if (ip)      ip[0] = '\0';
    if (state)   state[0] = '\0';
    if (wpa_cmd_status("STATUS", buf, sizeof(buf)) != 0) return -1;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "wpa_state=", 10) == 0 && state) {
            strncpy(state, line + 10, state_sz - 1);
            state[state_sz - 1] = '\0';
        } else if (strncmp(line, "ssid=", 5) == 0 && ssid) {
            strncpy(ssid, line + 5, ssid_sz - 1);
            ssid[ssid_sz - 1] = '\0';
        } else if (strncmp(line, "ip_address=", 11) == 0 && ip) {
            strncpy(ip, line + 11, ip_sz - 1);
            ip[ip_sz - 1] = '\0';
        }
    }
    return 0;
}
