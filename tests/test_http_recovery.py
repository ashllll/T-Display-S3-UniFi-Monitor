#!/usr/bin/env python3
"""Execute the real HTTP wrapper against a transport stuck after header timeout."""
from pathlib import Path
import subprocess
import tempfile
import os

SRC = Path(__file__).resolve().parents[1] / 'examples/ikuai_widget/src'
source = Path(os.environ.get('UNIFI_HTTP_TEST_SOURCE', SRC / 'unifi_monitor.c')).read_text()
start = source.index('static esp_err_t on_http_evt(')
wrapper = source[start:source.index('// Pure typed JSON helpers')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include "unifi_monitor.h"
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_HTTP_EAGAIN 2
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_EVENT_ON_DATA 2
#define HTTP_METHOD_GET 0
#define HTTP_TIMEOUT_MS 5000
#define API_PREFIX "/test"
#define UNIFI_HOST "https://example.invalid"
#define UNIFI_API_KEY "synthetic-test-key"
#define UNIFI_TLS_SERVER_NAME "example.invalid"
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct {int event_id; char *header_key,*header_value; void *user_data,*data; int data_len;} esp_http_client_event_t;
typedef struct {
 const char *url,*cert_pem,*common_name,*user_agent;
 int method,timeout_ms;
 bool skip_cert_common_name_check,disable_auto_redirect;
 void (*crt_bundle_attach)(void);
 esp_err_t (*event_handler)(esp_http_client_event_t *);
 void *user_data;
} esp_http_client_config_t;
typedef struct {bool stuck; esp_http_client_config_t cfg;} client_t;
typedef client_t *esp_http_client_handle_t;
typedef struct {char *buf; int len,cap; bool truncated;} resp_t;
static resp_t s_resp;
static esp_http_client_handle_t s_client;
static unifi_link_state_t s_link_state;
static bool s_network_changed;
static int64_t s_retry_at_ms,now_ms;
static const char unifi_cert_pem[]="";
static int requests, cleanups, result, status=200, setter_error;
static bool complete=true, network_during_request;
static char *retry_after;
static bool config_valid(void){return true;}
static int64_t esp_timer_get_time(void){return now_ms*1000;}
static void esp_crt_bundle_attach(void){}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){
 assert(!cfg->skip_cert_common_name_check && cfg->disable_auto_redirect);
 assert(cfg->crt_bundle_attach==esp_crt_bundle_attach);
 client_t *c=calloc(1,sizeof(*c)); assert(c); c->cfg=*cfg; return c;
}
static int esp_http_client_cleanup(client_t *c){cleanups++;free(c);return ESP_OK;}
static int esp_http_client_set_url(client_t *c,const char *url){(void)c;(void)url;return setter_error;}
static int esp_http_client_set_timeout_ms(client_t *c,int t){(void)c;assert(t>0);return 0;}
static int esp_http_client_set_header(client_t *c,const char *k,const char *v){(void)c;(void)k;(void)v;return 0;}
static int esp_http_client_get_status_code(client_t *c){(void)c;return status;}
static bool esp_http_client_is_complete_data_received(client_t *c){(void)c;return complete;}
static int esp_http_client_perform(client_t *c){
 // Model IDF 5.2.1: header timeout retains REQ_COMPLETE_DATA; retry reads
 // the old response without sending. Cleanup is necessary to send again.
 if(c->stuck)return ESP_ERR_HTTP_EAGAIN;
 requests++;
 if(result){c->stuck=true;return result;}
 if(network_during_request)s_network_changed=true;
 if(retry_after){esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,
 .header_key="Retry-After",.header_value=retry_after};c->cfg.event_handler(&e);}
 esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.data="{}",.data_len=2,.user_data=c->cfg.user_data};
 c->cfg.event_handler(&e);return 0;
}
''' + wrapper + r'''
static void recover(void){
 int before=requests;
 result=0;complete=true;network_during_request=false;s_network_changed=false;setter_error=0;
 char buf[16]; assert(api_get("/recovered",buf,sizeof(buf),5000));
 assert(requests==before+1 && !strcmp(buf,"{}") && s_link_state==UNIFI_LINK_ONLINE);
}
int main(void){
 char buf[16];
 recover();int initial=cleanups;recover();assert(cleanups==initial); // healthy reuse
 for(int i=1;i<=2;i++){
  result=i;assert(!api_get("/timeout",buf,sizeof(buf),5000));
  assert(s_link_state==UNIFI_LINK_TIMEOUT && !s_client);recover();
 }
 complete=false;assert(!api_get("/partial",buf,sizeof(buf),5000));assert(!s_client);recover();
 network_during_request=true;assert(!api_get("/network",buf,sizeof(buf),5000));assert(!s_client);recover();
 setter_error=1;assert(!api_get("/setup",buf,sizeof(buf),5000));assert(!s_client);recover();
 status=401;assert(!api_get("/auth",buf,sizeof(buf),5000));assert(s_link_state==UNIFI_LINK_AUTH_ERROR);
 status=429;retry_after="12";assert(!api_get("/limited",buf,sizeof(buf),5000));
 int before=requests;status=200;retry_after=NULL;
 assert(!api_get("/cooldown",buf,sizeof(buf),5000));assert(requests==before);
 now_ms=12000;recover();
 assert(!api_get("/overflow",buf,2,5000));assert(s_link_state==UNIFI_LINK_HTTP_ERROR);recover();
 assert(cleanups>=5);esp_http_client_cleanup(s_client);
 puts("PASS: fresh GET after timeout/incomplete body/network change/setup failure; healthy reuse/TLS/auth/429/buffer bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='unifi-http-') as tmp:
    c = Path(tmp) / 'test.c'
    c.write_text(code)
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-I', str(SRC), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
