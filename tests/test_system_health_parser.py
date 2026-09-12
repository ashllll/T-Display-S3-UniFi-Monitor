#!/usr/bin/env python3
"""Compile actual UniFi parsers with SDK cJSON and address/undefined sanitizers."""
import os
import sys
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "examples/ikuai_widget/src"
source = (SRC / "unifi_monitor.c").read_text()
def function(signature):
    return source[source.index(signature):].split('\n}',1)[0]+'\n}\n'
parser = function('static bool uuid_valid(') + source[source.index('static const cJSON *field('):source.index('// ─── Ping')]

STUBS = r'''

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include "cJSON.h"
#include "unifi_monitor.h"
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define HTTP_TIMEOUT_MS 5000
#define HEARTBEAT_MAX_AGE_SEC 90
static unifi_sys_t s_sys;
static unifi_extra_t s_extra;
static unifi_curve_t s_curve;
static unifi_link_state_t s_link_state;
static uint32_t s_last_ok_ts, s_heartbeat_seen;
static int s_mux=1;
static char s_site[37],s_device[37],s_heartbeat[40];
static int64_t now=42;
static int64_t esp_timer_get_time(void){return now*1000000;}
static bool live_mode;
static time_t test_time(time_t *out){time_t t=live_mode?time(NULL):1789171200;if(out)*out=t;return t;}
#define time test_time
static bool api_get(const char *a,char *b,int c,int d){(void)a;(void)b;(void)c;(void)d;return false;}
'''
CHECKS = r'''

#define STAMP "2026-09-12T00:00:00Z"
static void stats(const char *fields){
 char b[1024];snprintf(b,sizeof(b),"{\"interfaces\":{},\"lastHeartbeatAt\":\"%s\",%s}",STAMP,fields);
 assert(parse_system(b));
}
static char *read_sample(const char *dir,const char *name){
 char path[1024];snprintf(path,sizeof(path),"%s/%s.json",dir,name);FILE *f=fopen(path,"rb");assert(f);
 char *buf=calloc(32768,1);assert(buf);size_t n=fread(buf,1,32767,f);assert(n>0 && n<32767);fclose(f);return buf;
}
int main(int argc,char **argv){
 if(argc==2){
  live_mode=true;now=200;
  char *b=read_sample(argv[1],"sites");assert(parse_sites(b));free(b);
  b=read_sample(argv[1],"devices");cJSON *root=cJSON_Parse(b);const cJSON *first=cJSON_GetArrayItem(field(root,"data"),0);
  assert(string(first,"id",s_device,sizeof(s_device)) && uuid_valid(s_device));cJSON_Delete(root);
  inventory_t inv={0};uint32_t next,total;assert(parse_devices_page(b,0,&inv,&next,&total));free(b);
  assert(inv.selected_found && inv.selected_online && inv.rows>0);
  b=read_sample(argv[1],"stats0");assert(parse_system(b) && s_sys.cpu_valid && s_sys.mem_valid && s_sys.rate_valid);free(b);
  b=read_sample(argv[1],"clients");assert(parse_clients(b) && s_extra.online_valid);free(b);
  b=read_sample(argv[1],"wans");assert(parse_wan(b));free(b);
  printf("PASS LIVE: actual C parsers accepted selected device, CPU/MEM/uplink/heartbeat, %lu clients and %d WAN definitions\n",(unsigned long)s_extra.online_count,s_extra.wan_cnt);
  return 0;
 }

 stats("\"cpuUtilizationPct\":25.5,\"memoryUtilizationPct\":0,\"uptimeSec\":123,\"loadAverage1Min\":0.25,\"uplink\":{\"rxRateBps\":8000,\"txRateBps\":0}");
 assert(s_sys.ok && s_sys.rate_valid && s_sys.down_bps==1000 && s_sys.up_bps==0);
 assert(s_sys.cpu_valid && s_sys.cpu_pct==25.5 && s_sys.mem_valid && s_sys.mem_pct==0);
 assert(s_extra.load_valid[0] && s_extra.load_avg[0]==.25 && !s_extra.load_valid[1]);
 assert(s_extra.uptime_valid && s_extra.uptime_sec==123);
 assert(s_curve.n==1 && s_extra.system_ts==42);
 stats("\"cpuUtilizationPct\":101,\"memoryUtilizationPct\":null");
 assert(!s_sys.cpu_valid && !s_sys.mem_valid && !s_sys.rate_valid && !s_curve.n);
 stats("\"uplink\":{\"rxRateBps\":-1,\"txRateBps\":8}");assert(!s_sys.rate_valid);
 stats("\"uplink\":{\"rxRateBps\":1.5,\"txRateBps\":8}");assert(!s_sys.rate_valid);
 assert(!parse_system("{}") && !s_sys.ok);
 assert(!parse_system("{\"interfaces\":{},\"lastHeartbeatAt\":\"2026-09-11T23:00:00Z\"}"));
 assert(!parse_system("{\"interfaces\":{},\"lastHeartbeatAt\":\"2026-09-12T00:01:00Z\"}"));
 assert(!parse_system("{\"interfaces\":{},\"lastHeartbeatAt\":\"2026-02-30T00:00:00Z\"}"));
 assert(!parse_system("{\"interfaces\":{},\"lastHeartbeatAt\":\"" STAMP "\"} trailing"));
 now=133;assert(!parse_system("{\"interfaces\":{},\"lastHeartbeatAt\":\"" STAMP "\"}"));
 assert(s_link_state==UNIFI_LINK_STALE);now=42;s_heartbeat[0]=0;
 const char *id="00000000-0000-0000-0000-000000000001";
 assert(uuid_valid(id) && !uuid_valid("00000000-0000-0000-0000-000000000001x"));
 assert(parse_sites("{\"offset\":0,\"count\":1,\"totalCount\":1,\"data\":[{\"id\":\"00000000-0000-0000-0000-000000000001\"}]}"));assert(!strcmp(s_site,id));
 assert(!parse_sites("{\"offset\":0,\"count\":0,\"totalCount\":2,\"data\":[]}"));
 assert(parse_clients("{\"offset\":0,\"count\":3,\"totalCount\":23,\"data\":[{\"name\":\"Phone\",\"type\":\"WIRELESS\",\"ipAddress\":\"192.168.0.2\"},{\"name\":\"PC\",\"type\":\"WIRED\"},{\"name\":\"VPN\",\"type\":\"VPN\"}]}"));
 assert(s_extra.client_cnt==3 && s_extra.online_count==23 && s_extra.online_valid);
 assert(!strcmp(s_extra.client[0].ip,"192.168.0.2"));
 assert(!parse_clients("{\"offset\":0,\"count\":0,\"totalCount\":23,\"data\":[]}"));assert(s_extra.online_count==23);
 assert(parse_clients("{\"offset\":0,\"count\":0,\"totalCount\":0,\"data\":[]}"));assert(s_extra.online_valid && !s_extra.online_count);
 assert(!parse_clients("{\"data\":[]}"));
 assert(parse_wan("{\"offset\":0,\"count\":1,\"totalCount\":1,\"data\":[{\"name\":\"ISP 1\"}]}"));
 assert(s_extra.wan_cnt==1 && !strcmp(s_extra.wan[0].name,"ISP 1"));
 inventory_t inv={0};uint32_t next,total;
 assert(parse_devices_page("{\"offset\":0,\"count\":2,\"totalCount\":2,\"data\":[{\"id\":\"00000000-0000-0000-0000-000000000001\",\"features\":[\"gateway\"],\"state\":\"ONLINE\",\"firmwareVersion\":\"5.1.33\"},{\"id\":\"00000000-0000-0000-0000-000000000002\",\"features\":[\"accessPoint\"],\"state\":\"OFFLINE\"}]}",0,&inv,&next,&total));
 assert(inv.gateways==1 && inv.aps==1 && !inv.aps_online && inv.selected_online && next==2);
 assert(!parse_devices_page("{\"offset\":2,\"count\":0,\"totalCount\":3,\"data\":[]}",2,&inv,&next,&total));
 puts("PASS: UniFi stats, validity, heartbeat expiry, page counts, UUIDs, clients, AP inventory, WAN capability boundaries");
}
'''
# Reuse the SDK's cJSON source, not a test double or a downloaded copy.
if os.environ.get("CJSON_DIR"):
    cjson_dir = Path(os.environ["CJSON_DIR"])
else:
    packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
    candidates = sorted(packages.glob("framework-espidf*/components/json/cJSON/cJSON.c"))
    if not candidates:
        raise SystemExit("Set CJSON_DIR to the SDK directory containing cJSON.c and cJSON.h")
    cjson_dir = candidates[0].parent

with tempfile.TemporaryDirectory(prefix="unifi-health-test-") as tmp:
    c_file = Path(tmp) / "health.c"
    binary = Path(tmp) / "health"
    c_file.write_text(STUBS + parser + CHECKS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
                    "-I", str(SRC), "-I", str(cjson_dir), str(c_file),
                    str(cjson_dir / "cJSON.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    if len(sys.argv)==2:
        subprocess.run([str(binary),sys.argv[1]],check=True)
