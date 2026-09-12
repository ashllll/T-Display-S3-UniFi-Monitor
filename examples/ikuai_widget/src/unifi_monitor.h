#pragma once

#include <stdint.h>
#include <stdbool.h>

// UniFi Network Integration API with X-API-Key and verified TLS.
// Internal throughput is bytes/sec; device uplink bit rates are divided by 8.
// Missing fields are explicitly invalid. WAN definitions are not WAN health.

// Retain the full 120-second chart at 1 Hz, including boundary samples.
#define UNIFI_CURVE_MAX 128

typedef enum {
    UNIFI_LINK_WAIT = 0,
    UNIFI_LINK_ONLINE,
    UNIFI_LINK_NETWORK_ERROR,
    UNIFI_LINK_TIMEOUT,
    UNIFI_LINK_AUTH_ERROR,
    UNIFI_LINK_HTTP_ERROR,
    UNIFI_LINK_CONFIG_ERROR,
    UNIFI_LINK_RATE_LIMIT,
    UNIFI_LINK_STALE,
} unifi_link_state_t;

typedef struct {
    bool ok;
    uint32_t ts;
    bool cpu_valid;
    float cpu_pct;
    bool mem_valid;
    float mem_pct;
    bool online_valid;
    uint32_t online_cnt;
    bool rate_valid;
    uint32_t down_bps;      // Device uplink RX, bytes/sec
    uint32_t up_bps;        // Device uplink TX, bytes/sec
} unifi_sys_t;

typedef struct {
    bool ok;
    uint32_t ts;
    int head;                          // 下一个写入位置
    int n;                             // 已填充点数（<= UNIFI_CURVE_MAX）
    uint32_t sample_ts[UNIFI_CURVE_MAX];
    uint32_t down[UNIFI_CURVE_MAX];    // B/s
    uint32_t up[UNIFI_CURVE_MAX];      // B/s
} unifi_curve_t;

// ICMP results have their own clock; HTTP failures must not drop probes.
#define UNIFI_PING_MAX_AGE_SEC 3
#define UNIFI_PING_WINDOW_SEC 60
#define UNIFI_PING_MAX UNIFI_PING_WINDOW_SEC
typedef struct {
    int head;
    int n;
    float ms[UNIFI_PING_MAX];          // -1 is an actual timeout
    uint32_t ts[UNIFI_PING_MAX];
} unifi_ping_t;

bool unifi_get_ping(unifi_ping_t *out);

typedef struct {
    char name[48],model[24],ip[46],state[32];
    bool selected,access_point;
} unifi_device_t;

// Each extension has its own successful sample timestamp.
typedef struct { char name[40]; } unifi_wan_t;
typedef struct {
    char name[48];
    char ip[46]; // IPv4 or IPv6 presentation
    char interface[12]; // WIRED / WIRELESS / VPN
} unifi_client_t;

typedef struct {
    bool ok;
    bool device_valid, device_online;
    uint32_t device_ts;
    uint32_t device_count;
    int device_rows;
    unifi_device_t devices[3];
    char selected_name[48],selected_model[24],selected_ip[46];
    bool load_valid[3];
    float load_avg[3];
    bool ap_counts_valid;
    int ap_online, ap_count;
    bool online_valid;
    uint32_t online_count;
    uint32_t system_ts;
    bool uptime_valid;
    uint32_t uptime_sec;
    bool version_valid;
    char version[16];
    uint32_t wan_ts;
    int wan_cnt;
    unifi_wan_t wan[2];
    uint32_t clients_ts;
    int client_cnt;
    unifi_client_t client[3]; // First three results, not ranked by traffic
} unifi_extra_t;

bool unifi_get_extra(unifi_extra_t *out);

void unifi_monitor_start(void);
bool unifi_get_sys(unifi_sys_t *out);
bool unifi_get_curve(unifi_curve_t *out);
bool unifi_recently_ok(void);
unifi_link_state_t unifi_get_link_state(void);
void unifi_monitor_network_changed(void);
