// UniFi Network Integration API collector. See docs in README.md.
#include "unifi_monitor.h"
#if __has_include("unifi_cert.h")
#include "unifi_cert.h"
#else
#include "unifi_cert.example.h"
#endif
#if __has_include("unifi_config.h")
#include "unifi_config.h"
#endif
#include "unifi_config.example.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "apps/ping/ping_sock.h"   // IDF 6.0 无 esp_ping 组件，用 lwip ping_sock
#include "esp_netif.h"


static const char *TAG = "unifi";
#define HTTP_TIMEOUT_MS 5000
#define API_PREFIX "/proxy/network/integration/v1"
#define HEARTBEAT_MAX_AGE_SEC 90
static SemaphoreHandle_t s_mux;
static unifi_sys_t s_sys;
static unifi_extra_t s_extra;
static unifi_curve_t s_curve;
static unifi_ping_t s_ping_history;
static volatile uint32_t s_last_ok_ts;
static volatile unifi_link_state_t s_link_state = UNIFI_LINK_WAIT;
static volatile bool s_network_changed;
static char s_site[37], s_device[37];
static int64_t s_retry_at_ms;
static char s_heartbeat[40];
static uint32_t s_heartbeat_seen;

typedef struct { char *buf; int len, cap; bool truncated; } resp_t;
static resp_t s_resp;
static esp_http_client_handle_t s_client;

static bool uuid_valid(const char *id) {
    if (strlen(id) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!isxdigit((unsigned char)id[i])) return false;
    }
    return true;
}

static bool config_valid(void) {
    if (strncmp(UNIFI_HOST, "https://", 8) || !UNIFI_HOST[8] ||
        strpbrk(UNIFI_HOST + 8, "/?#@ \r\n") || !UNIFI_API_KEY[0] ||
        strpbrk(UNIFI_API_KEY, "\r\n") || strlen(UNIFI_API_KEY) > 512)
        return false;
    return (!UNIFI_SITE_ID[0] || uuid_valid(UNIFI_SITE_ID)) &&
           (!UNIFI_DEVICE_ID[0] || uuid_valid(UNIFI_DEVICE_ID));
}

static esp_err_t on_http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Retry-After") == 0) {
        char *end;
        unsigned long sec = strtoul(evt->header_value, &end, 10);
        if (end != evt->header_value && !*end) {
            if (sec > 300) sec = 300;
            s_retry_at_ms = esp_timer_get_time() / 1000 + (int64_t)(sec ? sec : 1) * 1000;
        }
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        resp_t *r = evt->user_data;
        int room = r->cap - r->len - 1;
        int copy = evt->data_len < room ? evt->data_len : room;
        if (copy > 0) {
            memcpy(r->buf + r->len, evt->data, copy);
            r->len += copy;
            r->buf[r->len] = 0;
        }
        if (copy != evt->data_len) r->truncated = true;
    }
    return ESP_OK;
}

// Only the polling task owns this handle, including recovery after Wi-Fi changes.
static void discard_http_client(void) {
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static bool api_get(const char *path, char *buf, int cap, int timeout_ms) {
    if (!config_valid()) { s_link_state = UNIFI_LINK_CONFIG_ERROR; return false; }
    if (esp_timer_get_time() / 1000 < s_retry_at_ms) {
        s_link_state = UNIFI_LINK_RATE_LIMIT; return false;
    }
    char url[384];
    int n = snprintf(url, sizeof(url), "%s%s%s", UNIFI_HOST, API_PREFIX, path);
    if (n <= 0 || n >= sizeof(url)) { s_link_state = UNIFI_LINK_CONFIG_ERROR; return false; }
    if (!s_client) {
        esp_http_client_config_t cfg = {
            .url = url, .method = HTTP_METHOD_GET, .timeout_ms = HTTP_TIMEOUT_MS,
            .cert_pem = unifi_cert_pem[0] ? unifi_cert_pem : NULL,
            .crt_bundle_attach = unifi_cert_pem[0] ? NULL : esp_crt_bundle_attach,
            .common_name = UNIFI_TLS_SERVER_NAME,
            .skip_cert_common_name_check = false,
            .disable_auto_redirect = true,
            .event_handler = on_http_evt, .user_data = &s_resp,
            .user_agent = "T-Display-S3-UniFi/1.0",
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) { s_link_state = UNIFI_LINK_NETWORK_ERROR; return false; }
    }
    s_resp = (resp_t){ .buf = buf, .cap = cap };
    buf[0] = 0;
    if (esp_http_client_set_url(s_client, url) != ESP_OK ||
        esp_http_client_set_timeout_ms(s_client, timeout_ms) != ESP_OK ||
        esp_http_client_set_header(s_client, "X-API-Key", UNIFI_API_KEY) != ESP_OK ||
        esp_http_client_set_header(s_client, "Accept", "application/json") != ESP_OK) {
        discard_http_client();
        s_link_state = UNIFI_LINK_HTTP_ERROR; return false;
    }
    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        // IDF can leave a timed-out request waiting for its old response headers.
        // Recreate it so the next attempt actually sends a fresh GET.
        discard_http_client();
        s_link_state = err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN ?
                       UNIFI_LINK_TIMEOUT : UNIFI_LINK_NETWORK_ERROR;
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        return false;
    }
    if (s_network_changed) {
        discard_http_client();
        s_link_state = UNIFI_LINK_NETWORK_ERROR; return false;
    }
    if (!esp_http_client_is_complete_data_received(s_client)) {
        discard_http_client();
        s_link_state = UNIFI_LINK_HTTP_ERROR;
        ESP_LOGW(TAG, "GET incomplete response");
        return false;
    }
    int status = esp_http_client_get_status_code(s_client);
    if (status != 200 || !s_resp.len || s_resp.truncated) {
        s_link_state = status == 401 || status == 403 ? UNIFI_LINK_AUTH_ERROR :
                       status == 429 ? UNIFI_LINK_RATE_LIMIT : UNIFI_LINK_HTTP_ERROR;
        if (status == 429 && s_retry_at_ms <= esp_timer_get_time() / 1000)
            s_retry_at_ms = esp_timer_get_time() / 1000 + 30000;
        ESP_LOGW(TAG, "GET rejected: HTTP %d, truncated=%d", status, s_resp.truncated);
        return false;
    }
    s_link_state = UNIFI_LINK_ONLINE;
    return true;
}

// Pure typed JSON helpers, shared by the host fixture tests.
static const cJSON *field(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool number(const cJSON *item, bool percent, double min, double max, double *out) {
    double v;
    if (cJSON_IsNumber(item)) v = item->valuedouble;
    else if (cJSON_IsString(item)) {
        char *end;
        const char *text = item->valuestring;
        v = strtod(text, &end);
        if (end == text) return false;
        if (percent && *end == '%') end++;
        if (*end) return false;
    } else return false;
    if (!isfinite(v) || v < min || v > max) return false;
    *out = v;
    return true;
}

static bool u32(const cJSON *object, const char *key, uint32_t *out) {
    double v;
    if (!number(field(object, key), false, 0, UINT32_MAX, &v) || floor(v) != v)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool string(const cJSON *object, const char *key, char *out, size_t cap) {
    const cJSON *item = field(object, key);
    if (!cJSON_IsString(item) || !item->valuestring[0]) return false;
    snprintf(out, cap, "%s", item->valuestring);
    return true;
}

static void mark_system_sample_failed(void) {
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_sys.ok = false;
    xSemaphoreGive(s_mux);
}


// Packet rates are not link speed. API *RateBps are bit/s, UI stores byte/s.
static bool rate(const cJSON *obj, const char *key, uint32_t *bytes) {
    double v;
    if (!number(field(obj,key),false,0,(double)UINT32_MAX * 8,&v) || floor(v)!=v)
        return false;
    *bytes = (uint32_t)(v / 8.0);
    return true;
}

// Reject old/missing heartbeats, including successful HTTP replies with cached data.
static bool heartbeat_fresh(const cJSON *root, uint32_t now) {
    char stamp[40];
    if (!string(root,"lastHeartbeatAt",stamp,sizeof(stamp))) return false;
    int y,mo,d,h,mi,se,used=0;
    if (sscanf(stamp,"%d-%d-%dT%d:%d:%d%n",&y,&mo,&d,&h,&mi,&se,&used)!=6 ||
        y<2020 || y>2100 || mo<1 || mo>12 || d<1 || d>31 || h<0 || h>23 ||
        mi<0 || mi>59 || se<0 || se>59) return false;
    const char *end = stamp+used;
    if (*end=='.') { end++; if (!isdigit((unsigned char)*end)) return false;
        while (isdigit((unsigned char)*end)) end++; }
    if (strcmp(end,"Z") && strcmp(end,"+00:00")) return false;
    const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=(y%4==0 && (y%100!=0 || y%400==0));
    if(d>months[mo-1]+(mo==2 && leap))return false;
    int64_t days=0;
    for(int yr=1970;yr<y;yr++)days+=365+(yr%4==0 && (yr%100!=0 || yr%400==0));
    for(int m=1;m<mo;m++)days+=months[m-1]+(m==2 && leap);
    int64_t remote=(days+d-1)*86400+h*3600+mi*60+se;
    time_t wall=time(NULL);
    if (wall > 1700000000 && (remote > wall+30 || wall-remote > HEARTBEAT_MAX_AGE_SEC)) return false;
    if (strcmp(stamp,s_heartbeat)) {
        snprintf(s_heartbeat,sizeof(s_heartbeat),"%s",stamp); s_heartbeat_seen=now;
    }
    return now-s_heartbeat_seen <= HEARTBEAT_MAX_AGE_SEC;
}

static bool parse_system(const char *resp) {
    cJSON *root=cJSON_ParseWithOpts(resp,NULL,true);
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000000);
    if (!cJSON_IsObject(root) || !cJSON_IsObject(field(root,"interfaces"))) {
        cJSON_Delete(root); mark_system_sample_failed(); return false;
    }
    if (!heartbeat_fresh(root,now)) {
        cJSON_Delete(root); mark_system_sample_failed(); s_link_state=UNIFI_LINK_STALE; return false;
    }
    unifi_sys_t s={ .ok=true,.ts=now };
    double v;
    s.cpu_valid=number(field(root,"cpuUtilizationPct"),false,0,100,&v);
    if (s.cpu_valid) s.cpu_pct=v;
    s.mem_valid=number(field(root,"memoryUtilizationPct"),false,0,100,&v);
    if (s.mem_valid) s.mem_pct=v;
    const cJSON *uplink=field(root,"uplink");
    bool rx=rate(uplink,"rxRateBps",&s.down_bps), tx=rate(uplink,"txRateBps",&s.up_bps);
    s.rate_valid=rx&&tx;
    uint32_t uptime=0;
    bool uptime_valid=u32(root,"uptimeSec",&uptime);
    const char *keys[]={"loadAverage1Min","loadAverage5Min","loadAverage15Min"};
    bool valid[3]; float load[3]={0};
    for(int i=0;i<3;i++) {valid[i]=number(field(root,keys[i]),false,0,100000,&v);if(valid[i])load[i]=v;}
    cJSON_Delete(root);
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_sys=s; s_last_ok_ts=now; s_extra.ok=true; s_extra.system_ts=now;
    s_extra.uptime_valid=uptime_valid; s_extra.uptime_sec=uptime;
    memcpy(s_extra.load_valid,valid,sizeof(valid));memcpy(s_extra.load_avg,load,sizeof(load));
    if(s.rate_valid) {
        int h=s_curve.head; s_curve.down[h]=s.down_bps;s_curve.up[h]=s.up_bps;
        s_curve.sample_ts[h]=now;
        s_curve.head=(h+1)%UNIFI_CURVE_MAX;if(s_curve.n<UNIFI_CURVE_MAX)s_curve.n++;
        s_curve.ts=now;
    } else memset(&s_curve,0,sizeof(s_curve));
    xSemaphoreGive(s_mux);
    return true;
}

static bool page(const cJSON *root, const cJSON **data, uint32_t *total, uint32_t *offset) {
    uint32_t count;
    *data=field(root,"data");
    return cJSON_IsArray(*data) && u32(root,"totalCount",total) &&
        u32(root,"offset",offset) && u32(root,"count",&count) &&
        count==(uint32_t)cJSON_GetArraySize(*data) && *offset<=*total && count<=*total-*offset;
}

static bool parse_sites(const char *resp) {
    cJSON *root=cJSON_ParseWithOpts(resp,NULL,true);const cJSON *data;
    uint32_t total,off;char id[64]="";
    bool ok=page(root,&data,&total,&off) && off==0 && total==1 && cJSON_GetArraySize(data)==1 &&
        string(cJSON_GetArrayItem(data,0),"id",id,sizeof(id)) && uuid_valid(id);
    cJSON_Delete(root);
    if(ok)memcpy(s_site,id,sizeof(s_site));else s_link_state=UNIFI_LINK_CONFIG_ERROR;
    return ok;
}

typedef struct {
    int gateways, aps, aps_online;
    bool selected_found, selected_online;
    char gateway[37],version[16];
    int rows;unifi_device_t devices[3],selected;
} inventory_t;

static bool has_feature(const cJSON *obj,const char *name) {
    const cJSON *f; cJSON_ArrayForEach(f,field(obj,"features"))
        if(cJSON_IsString(f) && !strcmp(f->valuestring,name))return true;
    return false;
}

static bool parse_devices_page(const char *resp, uint32_t offset, inventory_t *inv,
                               uint32_t *next, uint32_t *total) {
    cJSON *root=cJSON_ParseWithOpts(resp,NULL,true);const cJSON *data,*obj;uint32_t off;
    bool ok=page(root,&data,total,&off) && off==offset;
    if(!ok){cJSON_Delete(root);return false;}
    *next=offset+(uint32_t)cJSON_GetArraySize(data);
    if(*next==offset && *next<*total){cJSON_Delete(root);return false;}
    cJSON_ArrayForEach(obj,data) {
        char id[64]="",state[40]="";
        if(!string(obj,"id",id,sizeof(id)) || !uuid_valid(id) ||
           !string(obj,"state",state,sizeof(state)) || !cJSON_IsArray(field(obj,"features"))) {ok=false;break;}
        bool online=!strcmp(state,"ONLINE");
        unifi_device_t row={0};
        string(obj,"name",row.name,sizeof(row.name));string(obj,"model",row.model,sizeof(row.model));
        string(obj,"ipAddress",row.ip,sizeof(row.ip));string(obj,"state",row.state,sizeof(row.state));
        row.access_point=has_feature(obj,"accessPoint");row.selected=s_device[0] && !strcmp(id,s_device);
        if(inv->rows<3)inv->devices[inv->rows++]=row;
        if(has_feature(obj,"accessPoint")){inv->aps++;if(online)inv->aps_online++;}
        if(has_feature(obj,"gateway")) {
            inv->gateways++;memcpy(inv->gateway,id,sizeof(inv->gateway));
        }
        if((s_device[0] && !strcmp(id,s_device)) || (!s_device[0] && has_feature(obj,"gateway"))) {
            inv->selected=row;inv->selected.selected=true;
            inv->selected_found=true;inv->selected_online=online;
            string(obj,"firmwareVersion",inv->version,sizeof(inv->version));
        }
    }
    cJSON_Delete(root);return ok;
}

static bool poll_devices(char *buf,int cap) {
    inventory_t inv={0};uint32_t offset=0,total=0,next=0,expected=UINT32_MAX;
    do {
        char path[160];snprintf(path,sizeof(path),"/sites/%s/devices?offset=%lu&limit=25",s_site,(unsigned long)offset);
        if(!api_get(path,buf,cap,HTTP_TIMEOUT_MS) || !parse_devices_page(buf,offset,&inv,&next,&total))return false;
        if(expected!=UINT32_MAX && total!=expected)return false;
        expected=total;offset=next;
        // Bounded inventory: fail visibly, never report a partial AP count as total.
        if(total>1000)return false;
    } while(offset<total);
    if(!s_device[0]) {
        if(inv.gateways!=1){s_link_state=UNIFI_LINK_CONFIG_ERROR;return false;}
        memcpy(s_device,inv.gateway,sizeof(s_device));
    }
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_extra.ok=true;s_extra.device_valid=inv.selected_found;s_extra.device_online=inv.selected_online;
    s_extra.device_ts=(uint32_t)(esp_timer_get_time()/1000000);
    s_extra.version_valid=inv.selected_found && inv.version[0];memcpy(s_extra.version,inv.version,sizeof(inv.version));
    s_extra.device_count=total;s_extra.device_rows=inv.rows;
    memcpy(s_extra.devices,inv.devices,sizeof(inv.devices));
    memcpy(s_extra.selected_name,inv.selected.name,sizeof(inv.selected.name));
    memcpy(s_extra.selected_model,inv.selected.model,sizeof(inv.selected.model));
    memcpy(s_extra.selected_ip,inv.selected.ip,sizeof(inv.selected.ip));
    s_extra.ap_counts_valid=true;s_extra.ap_count=inv.aps;s_extra.ap_online=inv.aps_online;
    xSemaphoreGive(s_mux);
    return true;
}

static bool parse_clients(const char *resp) {
    cJSON *root=cJSON_ParseWithOpts(resp,NULL,true);const cJSON *data,*obj;uint32_t total,off;
    if(!page(root,&data,&total,&off) || off!=0){cJSON_Delete(root);return false;}
    unifi_client_t clients[3]={0};int n=0;
    cJSON_ArrayForEach(obj,data) {
        if(n==3)break;
        if(!cJSON_IsObject(obj) || !string(obj,"name",clients[n].name,sizeof(clients[n].name)) ||
            !string(obj,"type",clients[n].interface,sizeof(clients[n].interface))){cJSON_Delete(root);return false;}
        string(obj,"ipAddress",clients[n].ip,sizeof(clients[n].ip));n++;
    }
    if((uint32_t)n != (total<3?total:3)){cJSON_Delete(root);return false;}
    cJSON_Delete(root);
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_extra.ok=true;s_extra.client_cnt=n;s_extra.online_valid=true;s_extra.online_count=total;
    memcpy(s_extra.client,clients,sizeof(clients));s_extra.clients_ts=(uint32_t)(esp_timer_get_time()/1000000);
    xSemaphoreGive(s_mux);return true;
}

static bool parse_wan(const char *resp) {
    cJSON *root=cJSON_ParseWithOpts(resp,NULL,true);const cJSON *data,*obj;uint32_t total,off;
    if(!page(root,&data,&total,&off) || off!=0){cJSON_Delete(root);return false;}
    unifi_wan_t w[2]={0};int n=0;
    cJSON_ArrayForEach(obj,data) {
        if(n==2)break;
        if(!string(obj,"name",w[n].name,sizeof(w[n].name))){cJSON_Delete(root);return false;}n++;
    }
    if((uint32_t)n != (total<2?total:2)){cJSON_Delete(root);return false;}
    cJSON_Delete(root);
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_extra.ok=true;s_extra.wan_cnt=n;memcpy(s_extra.wan,w,sizeof(w));
    s_extra.wan_ts=(uint32_t)(esp_timer_get_time()/1000000);
    xSemaphoreGive(s_mux);return true;
}

// ─── Ping（ICMP 网关，1s 采样）──────────────────────────────────────

static esp_ping_handle_t s_ping = NULL;

static void record_ping(esp_ping_handle_t session, float ms) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (session == s_ping && !s_network_changed) {
        int last = (s_ping_history.head + UNIFI_PING_MAX - 1) % UNIFI_PING_MAX;
        if (s_ping_history.n && now - s_ping_history.ts[last] > UNIFI_PING_MAX_AGE_SEC)
            memset(&s_ping_history, 0, sizeof(s_ping_history));
        int h = s_ping_history.head;
        s_ping_history.ms[h] = ms;
        s_ping_history.ts[h] = now;
        s_ping_history.head = (h + 1) % UNIFI_PING_MAX;
        if (s_ping_history.n < UNIFI_PING_MAX) s_ping_history.n++;
    }
    xSemaphoreGive(s_mux);
}

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint32_t t = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    record_ping(hdl, (float)t);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    record_ping(hdl, -1);
}

static bool ping_start(void) {
    ip_addr_t gw;
    esp_netif_ip_info_t ipi;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ipi) != ESP_OK ||
        ipi.ip.addr == 0 || ipi.gw.addr == 0) return false;
    ip_addr_copy_from_ip4(gw, ipi.gw);     // 用实际网关
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = gw;
    cfg.count = ESP_PING_COUNT_INFINITE;   // 持续 ping（1s 间隔实时曲线）
    cfg.interval_ms = 1000;                // 1s 采样
    cfg.timeout_ms = 800;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };
    esp_ping_handle_t session = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &session) == ESP_OK) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_ping = session;
        xSemaphoreGive(s_mux);
        if (esp_ping_start(session) != ESP_OK) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_ping = NULL;
            xSemaphoreGive(s_mux);
            esp_ping_delete_session(session);
            return false;
        }
        ESP_LOGI(TAG, "ping started (1s interval)");
        return true;
    }
    return false;
}


// Only GET requests. Statistics every 2 seconds; one extension every 5 seconds.
static void poll_task(void *arg) {
    enum { BUF_SZ=32768 };
    char *buf=heap_caps_malloc(BUF_SZ,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!buf){s_link_state=UNIFI_LINK_NETWORK_ERROR;vTaskDelete(NULL);return;}
    int64_t next=0,next_ext=0;unsigned failures=0;int slot=0;
    for(;;) {
        if(s_network_changed) {
            discard_http_client();
            xSemaphoreTake(s_mux,portMAX_DELAY);
            // Retire the identity before asynchronous stop/delete. An old
            // in-flight callback must not enter the next network's history.
            esp_ping_handle_t retired = s_ping;
            s_ping = NULL;
            memset(&s_ping_history,0,sizeof(s_ping_history));
            memset(&s_extra,0,sizeof(s_extra));memset(&s_curve,0,sizeof(s_curve));
            s_network_changed=false;
            xSemaphoreGive(s_mux);
            if(retired){esp_ping_stop(retired);esp_ping_delete_session(retired);}
            mark_system_sample_failed();s_site[0]=s_device[0]=s_heartbeat[0]=0;
            next=next_ext=0;failures=0;
        }
        if(!s_ping)ping_start();
        int64_t now=esp_timer_get_time()/1000;
        if(now<next || now<s_retry_at_ms){vTaskDelay(pdMS_TO_TICKS(50));continue;}
        if(!config_valid()){s_link_state=UNIFI_LINK_CONFIG_ERROR;next=now+5000;continue;}
        bool ok=true;
        if(!s_site[0]) {
            if(UNIFI_SITE_ID[0])snprintf(s_site,sizeof(s_site),"%s",UNIFI_SITE_ID);
            else ok=api_get("/sites?offset=0&limit=2",buf,BUF_SZ,HTTP_TIMEOUT_MS)&&parse_sites(buf);
        }
        if(ok && !s_device[0]) {
            if(UNIFI_DEVICE_ID[0])snprintf(s_device,sizeof(s_device),"%s",UNIFI_DEVICE_ID);
            ok=poll_devices(buf,BUF_SZ);
        }
        if(ok) {
            char path[160];snprintf(path,sizeof(path),"/sites/%s/devices/%s/statistics/latest",s_site,s_device);
            now=esp_timer_get_time()/1000;
            ok=api_get(path,buf,BUF_SZ,HTTP_TIMEOUT_MS)&&parse_system(buf);
        }
        if(!ok) {
            mark_system_sample_failed();if(s_link_state==UNIFI_LINK_ONLINE)s_link_state=UNIFI_LINK_HTTP_ERROR;
            failures=failures<4?failures+1:4;
            next=esp_timer_get_time()/1000+(2000LL<<failures);
            continue;
        }
        // Poll on a one-second cadence; inventory work must not add a second wait.
        failures=0;next=now+1000;
        now=esp_timer_get_time()/1000;
        if(now>=next_ext) {
            unifi_link_state_t state=s_link_state;char path[160];bool ext_ok;
            switch(slot) {
            case 0:
                snprintf(path,sizeof(path),"/sites/%s/clients?offset=0&limit=3",s_site);
                ext_ok=api_get(path,buf,BUF_SZ,HTTP_TIMEOUT_MS)&&parse_clients(buf);break;
            case 1:
                snprintf(path,sizeof(path),"/sites/%s/wans?offset=0&limit=2",s_site);
                ext_ok=api_get(path,buf,BUF_SZ,HTTP_TIMEOUT_MS)&&parse_wan(buf);break;
            default:ext_ok=poll_devices(buf,BUF_SZ);break;
            }
            if(!ext_ok)ESP_LOGW(TAG,"extension %d unavailable",slot);
            if(s_link_state!=UNIFI_LINK_RATE_LIMIT && s_link_state!=UNIFI_LINK_AUTH_ERROR)s_link_state=state;
            slot=(slot+1)%3;next_ext=esp_timer_get_time()/1000+5000;

        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── 对外接口 ───────────────────────────────────────────────────────

static void *json_alloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void unifi_monitor_start(void) {
    cJSON_Hooks hooks = { .malloc_fn = json_alloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return;
    if (xTaskCreate(poll_task, "unifi", 8192, NULL, 4, NULL) != pdPASS)
        s_link_state = UNIFI_LINK_NETWORK_ERROR;
    ESP_LOGI(TAG, "unifi monitor started");
}

void unifi_monitor_network_changed(void) {
    if (!s_mux) { s_network_changed = true; return; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_network_changed = true;
    memset(&s_ping_history, 0, sizeof(s_ping_history));
    s_sys.ok = false;
    memset(&s_extra, 0, sizeof(s_extra));
    memset(&s_curve, 0, sizeof(s_curve));
    xSemaphoreGive(s_mux);
}

bool unifi_get_sys(unifi_sys_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_sys;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    out->online_valid = s_extra.online_valid && now - s_extra.clients_ts < 30;
    out->online_cnt = s_extra.online_count;
    xSemaphoreGive(s_mux);
    return out->ok && unifi_recently_ok();
}

bool unifi_get_curve(unifi_curve_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_curve;
    xSemaphoreGive(s_mux);
    return out->n > 0;
}

bool unifi_get_ping(unifi_ping_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_network_changed) *out = s_ping_history;
    xSemaphoreGive(s_mux);
    if (!out->n) return false;
    int last = (out->head + UNIFI_PING_MAX - 1) % UNIFI_PING_MAX;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    return now - out->ts[last] <= UNIFI_PING_MAX_AGE_SEC;
}

bool unifi_get_extra(unifi_extra_t *out) {
    if (!s_mux) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_extra;
    xSemaphoreGive(s_mux);
    return out->ok;
}

bool unifi_recently_ok(void) {
    if (!s_last_ok_ts) return false;
    return (uint32_t)(esp_timer_get_time() / 1000000) - s_last_ok_ts < 10;
}

unifi_link_state_t unifi_get_link_state(void) {
    return s_link_state;
}
