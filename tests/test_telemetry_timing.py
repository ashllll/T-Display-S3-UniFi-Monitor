#!/usr/bin/env python3
"""Host checks of actual ICMP history and UniFi scheduler code with fake time/I/O."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'examples/ikuai_widget/src'
monitor = (SRC / 'unifi_monitor.c').read_text()
ui = (SRC / 'desktop_widget.c').read_text()


def function(source, signature):
    return source[source.index(signature):].split('\n}', 1)[0] + '\n}\n'


stubs = r'''
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "unifi_monitor.h"
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define UNIFI_SITE_ID ""
#define UNIFI_DEVICE_ID ""
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define HTTP_TIMEOUT_MS 5000
#define pdMS_TO_TICKS(x) (x)
static int s_mux = 1;
static void *s_ping;
static bool s_network_changed;
static unifi_sys_t s_sys;
static unifi_extra_t s_extra;
static unifi_ping_t s_ping_history;
static unifi_link_state_t s_link_state;
static int64_t now_ms, s_retry_at_ms;
static char s_site[37], s_device[37], s_heartbeat[40];
static unifi_curve_t s_curve;
static bool config_valid(void){return true;}
static bool parse_sites(const char *s){(void)s;strcpy(s_site,"site");return true;}
static int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static jmp_buf done;
static void *scratch;
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return scratch = malloc(n); }
static void vTaskDelete(void *arg) { (void)arg; abort(); }
static void vTaskDelay(int ticks) { now_ms += ticks; if (now_ms >= 65000) longjmp(done, 1); }
static void esp_ping_stop(void *h) { (void)h; }
static void esp_ping_delete_session(void *h) { (void)h; }
static bool ping_start(void) { s_ping = &s_mux; return true; }
bool unifi_recently_ok(void) { return s_sys.ok; }
static bool parse_system(const char *s) { (void)s; s_sys.ok = true; return true; }
static bool parse_wan(const char *s) { (void)s; return true; }
static bool parse_clients(const char *s) { (void)s; return true; }
typedef struct { int kind, timeout; int64_t start; } request_t;
static request_t requests[200];
static int request_count, scenario;
static bool api_get(const char *path, char *buf, int cap, int timeout) {
    (void)buf; (void)cap;
    int kind = strstr(path, "statistics/latest") ? 0 : strstr(path,"/wans?") ? 1 :
        strstr(path,"/clients?") ? 2 : strstr(path,"/devices?") ? 3 : 4;
    assert(request_count < 200);
    requests[request_count++] = (request_t){kind, timeout, now_ms};
    if (scenario == 1 && kind == 0) {
        now_ms += 5000;
        s_link_state = UNIFI_LINK_TIMEOUT;
        return false;
    }
    if(scenario==2 && kind==0 && request_count==3){
        s_link_state=UNIFI_LINK_RATE_LIMIT;s_retry_at_ms=now_ms+30000;return false;
    }
    now_ms += kind == 0 ? 250 : (scenario == 0 && kind == 1 ? timeout : 100);
    s_link_state = kind == 1 && scenario == 0 ? UNIFI_LINK_TIMEOUT : UNIFI_LINK_ONLINE;
    return s_link_state == UNIFI_LINK_ONLINE;
}
static bool poll_devices(char *buf,int cap){
    if(!api_get("/devices?",buf,cap,HTTP_TIMEOUT_MS))return false;
    strcpy(s_device,"device");return true;
}
'''
code = stubs + '\n'.join(function(monitor, sig) for sig in [
    'static void mark_system_sample_failed(', 'static void record_ping(',
    'void unifi_monitor_network_changed(', 'bool unifi_get_ping(', 'static void poll_task('])

code += r'''
int main(void) {
    unifi_ping_t p;
    now_ms = 10000;
    assert(!unifi_get_ping(&p));
    // No HTTP samples at all: all ten ICMP results still enter the ring.
    for (int i = 0; i < 10; i++) {
        now_ms = 10000 + i * 1000;
        record_ping(i == 4 ? -1 : 10);
    }
    assert(unifi_get_ping(&p) && p.n == 10 && !s_sys.ok);
    now_ms = 22000;
    assert(unifi_get_ping(&p));
    now_ms = 23000;
    assert(!unifi_get_ping(&p));
    record_ping(25);
    assert(unifi_get_ping(&p) && p.n == 1 && p.ms[0] == 25);
    unifi_monitor_network_changed();
    assert(!unifi_get_ping(&p) && !p.n);
    record_ping(12); // Late callback from the old network is ignored.
    assert(!s_ping_history.n);
    s_network_changed = false;
    for (int i = 0; i < 70; i++) { now_ms += 1000; record_ping(i); }
    assert(unifi_get_ping(&p) && p.n == 60 && p.ms[(p.head+59)%60] == 69);
    for (scenario = 0; scenario < 3; scenario++) {
        now_ms = 1000; request_count = 0; s_sys.ok = false;
        s_site[0]=s_device[0]=0;s_retry_at_ms=0;
        s_link_state = UNIFI_LINK_WAIT;
        if (!setjmp(done)) poll_task(NULL);
        free(scratch);
        int extensions=0,main_count=0;int64_t last_main=0;
        for(int i=0;i<request_count;i++){
            request_t r=requests[i];
            if(r.kind==0){
                if(last_main){
                    assert(r.start-last_main>=1000);
                    if(scenario==0)assert(r.start-last_main<=5500);
                    if(scenario==1)assert(r.start-last_main>=9000);
                    if(scenario==2 && main_count==1)assert(r.start-last_main>=30000);
                }
                main_count++;last_main=r.start;
            } else if(r.kind==1 || r.kind==2){extensions++;assert(r.timeout==5000);}
        }
        assert(main_count>0);
        if(scenario==1)assert(!extensions && main_count<6);
        else assert(extensions>0);
    }
    puts("PASS: ICMP independent sampling/expiry/reconnect/wrap and polling spacing/exponential backoff/429 cooldown");
}
'''

with tempfile.TemporaryDirectory(prefix='unifi-timing-') as tmp:
    for name, source in [('timing', code)]:
        c = Path(tmp) / (name + '.c')
        binary = Path(tmp) / name
        c.write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-I', str(SRC), str(c), '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
