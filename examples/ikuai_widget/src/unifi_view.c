// Compact UniFi Network console: navigation rail, inventory, tables and activity.
#include "unifi_view.h"
#include "unifi_monitor.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

LV_FONT_DECLARE(ui_font_lato_12);
LV_FONT_DECLARE(ui_font_lato_20);
#define META (&ui_font_lato_12)
#define TITLE (&ui_font_lato_20)
#define BG 0x111315
#define PANEL 0x191C1F
#define HEADER 0x25282C
#define BORDER 0x30343A
#define TEXT 0xEBEDF0
#define MUTED 0x9299A3
#define BLUE 0x579AFF
#define PURPLE 0xAC8CEB
#define GREEN 0x73B880
#define AMBER 0xD9B56D
#define RED 0xE67E80
#define VIEWS 5
#define WINDOW_SEC 120
_Static_assert(UNIFI_CURVE_MAX > WINDOW_SEC, "1 Hz history must cover the full chart window");
static const char *titles[VIEWS]={"Overview","Devices","Clients","Traffic","Console"};
static lv_obj_t *pages[VIEWS],*nav[VIEWS],*nav_icon[VIEWS],*title,*footer,*status;
static lv_obj_t *hero_name,*hero_model,*hero_state,*counts[2],*overview_rates[2];
static lv_obj_t *device_name[3],*device_model[3],*device_state[3],*device_shape[3];
static lv_obj_t *client_name[3],*client_ip[3],*client_type[3],*device_count,*client_count,*device_empty,*client_empty,*client_shape[3];
static lv_obj_t *rates[2],*traffic_scale,*system_name,*system_version,*cpu,*memory,*load,*uptime,*system_ip;
static lv_obj_t *bars[2];
static int selected;
static uint32_t last_chart_ts;
static bool last_chart_valid;
static unifi_curve_t animation_curve;
static int32_t animation_scale=1000,scale_target=1000;
static uint32_t scale_tick_ms;
typedef struct {lv_obj_t *obj,*empty;lv_chart_series_t *rx,*tx;} activity_t;
static activity_t mini,traffic;

static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius){
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);return o;
}
static lv_obj_t *label(lv_obj_t *p,int x,int y,int w,const lv_font_t *font,uint32_t color,const char *text){
    lv_obj_t *o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_label_set_text(o,text);return o;
}
static void text(lv_obj_t *o,const char *s){if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
static void visible(lv_obj_t *o,bool show){if(show)lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);}
static void tint(lv_obj_t *o,uint32_t c){if(!lv_color_eq(lv_obj_get_style_text_color(o,0),lv_color_hex(c)))lv_obj_set_style_text_color(o,lv_color_hex(c),0);}
static bool fresh(uint32_t now,uint32_t stamp){return stamp && now>=stamp && now-stamp<30;}
static void glyph(lv_obj_t *p,int kind,uint32_t color){
    if(kind==0){for(int i=0;i<4;i++)box(p,4+(i%2)*7,5+(i/2)*7,5,5,color,1);}
    else if(kind==1){lv_obj_t *o=box(p,4,5,12,12,PANEL,8);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(color),0);box(p,9,10,2,2,color,1);}
    else if(kind==2){lv_obj_t *o=box(p,3,5,14,10,PANEL,1);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(color),0);box(p,7,17,6,1,color,0);}
    else if(kind==3){for(int i=0;i<3;i++)box(p,4+i*5,14-i*4,3,5+i*4,color,1);}
    else {box(p,3,6,14,11,color,2);box(p,5,9,4,2,PANEL,0);box(p,11,9,4,2,PANEL,0);}
}
static void device_art(lv_obj_t *parent,int x,int y,bool ap){
    if(ap){lv_obj_t *o=box(parent,x+5,y,24,24,0xDDE1E5,14);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x7B8795),0);o=box(parent,x+12,y+7,10,10,0xDDE1E5,6);lv_obj_set_style_border_width(o,2,0);lv_obj_set_style_border_color(o,lv_color_hex(BLUE),0);}
    else {box(parent,x,y+5,34,17,0xC6CCD3,4);box(parent,x+2,y+5,30,12,0xEDF0F3,3);box(parent,x+14,y+9,6,3,BLUE,1);}
}
static activity_t chart(lv_obj_t *p,int x,int y,int w,int h,bool compact){
    activity_t a={0};a.obj=lv_chart_create(p);lv_obj_set_pos(a.obj,x,y);lv_obj_set_size(a.obj,w,h);
    lv_obj_set_style_bg_color(a.obj,lv_color_hex(PANEL),0);lv_obj_set_style_bg_opa(a.obj,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(a.obj,0,0);lv_obj_set_style_pad_all(a.obj,0,0);lv_obj_set_style_radius(a.obj,0,0);
    lv_obj_set_style_line_color(a.obj,lv_color_hex(BORDER),LV_PART_MAIN);lv_obj_set_style_line_width(a.obj,1,LV_PART_MAIN);
    lv_obj_set_style_line_width(a.obj,compact?1:2,LV_PART_ITEMS);lv_obj_set_style_size(a.obj,0,0,LV_PART_INDICATOR);
    lv_chart_set_type(a.obj,LV_CHART_TYPE_SCATTER);lv_chart_set_point_count(a.obj,UNIFI_CURVE_MAX);
    lv_chart_set_div_line_count(a.obj,compact?2:4,compact?0:5);
    lv_chart_set_range(a.obj,LV_CHART_AXIS_PRIMARY_X,0,WINDOW_SEC*1000);lv_chart_set_range(a.obj,LV_CHART_AXIS_PRIMARY_Y,0,1000);
    a.rx=lv_chart_add_series(a.obj,lv_color_hex(BLUE),LV_CHART_AXIS_PRIMARY_Y);
    a.tx=lv_chart_add_series(a.obj,lv_color_hex(PURPLE),LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(a.obj,a.rx,LV_CHART_POINT_NONE);lv_chart_set_all_value(a.obj,a.tx,LV_CHART_POINT_NONE);
    a.empty=label(p,x,y+h/2-7,w,META,MUTED,"Waiting for data");lv_obj_set_style_text_align(a.empty,LV_TEXT_ALIGN_CENTER,0);
    return a;
}
static void build_overview(lv_obj_t *p){
    device_art(p,4,3,false);
    hero_name=label(p,48,0,224,TITLE,TEXT,"UniFi Console");
    hero_model=label(p,48,25,133,META,MUTED,"Gateway");hero_state=label(p,190,25,80,META,MUTED,"Waiting");
    box(p,0,46,274,1,BORDER,0);
    counts[0]=label(p,4,54,40,TITLE,TEXT,"--");label(p,46,59,84,META,MUTED,"Devices");
    counts[1]=label(p,145,54,40,TITLE,TEXT,"--");label(p,191,59,80,META,MUTED,"Clients");
    box(p,136,54,1,23,BORDER,0);
    overview_rates[0]=label(p,4,81,137,META,BLUE,"RX --");overview_rates[1]=label(p,4,101,137,META,PURPLE,"TX --");
    mini=chart(p,146,81,122,35,true);
}
static void build_devices(lv_obj_t *p){
    device_empty=label(p,4,62,265,META,MUTED,"Device data unavailable");
    label(p,2,1,150,META,MUTED,"UniFi devices");device_count=label(p,190,1,80,META,MUTED,"-- adopted");
    box(p,0,20,274,1,BORDER,0);
    for(int i=0;i<3;i++){
        int y=27+i*31;device_shape[i]=box(p,2,y,38,29,PANEL,4);
        device_name[i]=label(p,48,y,148,META,TEXT,"--");device_model[i]=label(p,48,y+16,158,META,MUTED,"--");
        device_state[i]=label(p,199,y,74,META,MUTED,"--");
        if(i<2)box(p,48,y+29,226,1,BORDER,0);
    }
}
static void build_clients(lv_obj_t *p){
    client_empty=label(p,4,62,265,META,MUTED,"Client data unavailable");
    label(p,2,1,150,META,MUTED,"Connected clients");client_count=label(p,188,1,85,META,MUTED,"-- total");box(p,0,20,274,1,BORDER,0);
    for(int i=0;i<3;i++){
        int y=27+i*31;lv_obj_t *o=box(p,2,y+1,30,27,HEADER,4);glyph(o,2,MUTED);client_shape[i]=o;
        client_name[i]=label(p,40,y,168,META,TEXT,"--");client_ip[i]=label(p,40,y+16,216,META,MUTED,"--");
        client_type[i]=label(p,215,y,58,META,MUTED,"--");if(i<2)box(p,40,y+29,234,1,BORDER,0);
    }
}
static void build_traffic(lv_obj_t *p){
    label(p,1,0,130,META,BLUE,"Receive");label(p,141,0,128,META,PURPLE,"Transmit");
    rates[0]=label(p,1,16,136,TITLE,TEXT,"--");rates[1]=label(p,141,16,132,TITLE,TEXT,"--");
    label(p,1,44,144,META,MUTED,"Device uplink");traffic_scale=label(p,156,44,117,META,MUTED,"-- Mbps");
    lv_obj_set_style_text_align(traffic_scale,LV_TEXT_ALIGN_RIGHT,0);
    traffic=chart(p,1,59,270,41,false);label(p,1,104,98,META,MUTED,"2 min ago");
    lv_obj_t *o=label(p,194,104,78,META,MUTED,"Now");lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_RIGHT,0);
}
static void build_system(lv_obj_t *p){
    system_name=label(p,2,0,270,TITLE,TEXT,"Console");system_version=label(p,2,25,270,META,MUTED,"UniFi OS --");
    box(p,0,44,274,1,BORDER,0);label(p,2,51,68,META,MUTED,"CPU");label(p,143,51,74,META,MUTED,"Memory");
    cpu=label(p,72,51,59,META,TEXT,"--");memory=label(p,216,51,58,META,TEXT,"--");
    box(p,2,73,126,4,BORDER,2);box(p,143,73,126,4,BORDER,2);
    bars[0]=box(p,2,73,0,4,BLUE,2);bars[1]=box(p,143,73,0,4,BLUE,2);
    label(p,2,86,65,META,MUTED,"Uptime");uptime=label(p,77,86,192,META,TEXT,"--");
    label(p,2,105,65,META,MUTED,"Load");load=label(p,77,105,192,META,TEXT,"-- / -- / --");
    system_ip=label(p,2,130,270,META,MUTED,"");
}
static void select_view(int next){
    if(next<0 || next>=VIEWS)return;
    selected=next;
    for(int i=0;i<VIEWS;i++){
        if(i==selected)lv_obj_remove_flag(pages[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(pages[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(nav[i],lv_color_hex(i==selected?0x1C304D:BG),0);
        lv_obj_clean(nav_icon[i]);glyph(nav_icon[i],i,i==selected?BLUE:MUTED);
    }
    text(title,titles[selected]);lv_obj_invalidate(lv_screen_active());
}
void unifi_view_create(void){
    lv_obj_t *scr=lv_screen_active();lv_obj_remove_style_all(scr);lv_obj_set_style_bg_color(scr,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(scr,LV_OPA_COVER,0);lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    box(scr,0,0,320,25,HEADER,0);label(scr,8,1,22,TITLE,BLUE,"U");label(scr,39,6,61,META,BLUE,"Network");label(scr,100,6,10,META,MUTED,"/");
    title=label(scr,114,6,111,META,TEXT,"Overview");status=label(scr,236,6,80,META,MUTED,"Waiting");lv_obj_set_style_text_align(status,LV_TEXT_ALIGN_RIGHT,0);
    box(scr,29,25,1,145,BORDER,0);
    for(int i=0;i<VIEWS;i++){
        nav[i]=box(scr,3,30+i*27,23,24,BG,4);nav_icon[i]=box(nav[i],1,1,21,22,BG,0);lv_obj_set_style_bg_opa(nav_icon[i],LV_OPA_TRANSP,0);
        pages[i]=box(scr,38,32,274,136,BG,0);
    }
    build_overview(pages[0]);build_devices(pages[1]);build_clients(pages[2]);build_traffic(pages[3]);build_system(pages[4]);
    footer=label(scr,39,154,273,META,MUTED,"Connecting to UniFi...");select_view(0);
}
static const char *failure(bool wifi){
    if(!wifi)return "Waiting for Wi-Fi";
    switch(unifi_get_link_state()){
        case UNIFI_LINK_CONFIG_ERROR:return "Configure UniFi API access";
        case UNIFI_LINK_AUTH_ERROR:return "API key rejected";
        case UNIFI_LINK_RATE_LIMIT:return "API limit / retry scheduled";
        case UNIFI_LINK_STALE:return "Device heartbeat expired";
        case UNIFI_LINK_TIMEOUT:return "Console request timed out";
        default:return "Waiting for console data";
    }
}
static void format_rate(uint32_t bytes,char *out,size_t cap){
    double mbps=(double)bytes*8/1000000;
    if(mbps>=1000)snprintf(out,cap,"%.2f Gbps",mbps/1000);
    else if(mbps>=1)snprintf(out,cap,"%.1f Mbps",mbps);
    else snprintf(out,cap,"%.0f kbps",mbps*1000);
}
static void render_activity(activity_t *a,const unifi_curve_t *c,uint32_t now_ms,int32_t scale,bool valid){
    uint32_t now=now_ms/1000;
    lv_obj_update_layout(a->obj);
    int32_t width=lv_obj_get_content_width(a->obj);
    if(width<1)return;
    // Quantize the shared scroll offset, not each point's changing age.
    // Otherwise adjacent vertices move on different frames and stretch lines.
    int64_t scroll=(uint64_t)now_ms*width/(WINDOW_SEC*1000);
    lv_chart_set_range(a->obj,LV_CHART_AXIS_PRIMARY_X,0,width);
    int32_t *rx_x=lv_chart_get_x_array(a->obj,a->rx),*tx_x=lv_chart_get_x_array(a->obj,a->tx);
    int32_t *rx_y=lv_chart_get_y_array(a->obj,a->rx),*tx_y=lv_chart_get_y_array(a->obj,a->tx);
    for(int i=0;i<UNIFI_CURVE_MAX;i++){
        int32_t x=0,rx=LV_CHART_POINT_NONE,tx=LV_CHART_POINT_NONE;
        if(valid && i<c->n){int slot=(c->head-c->n+i+UNIFI_CURVE_MAX)%UNIFI_CURVE_MAX;
            uint32_t stamp=c->sample_ts[slot];
            bool gap=i && stamp-c->sample_ts[(slot+UNIFI_CURVE_MAX-1)%UNIFI_CURVE_MAX]>10;
            if(stamp<=now && now-stamp<=WINDOW_SEC && !gap){x=width+(int64_t)((uint64_t)stamp*width/WINDOW_SEC)-scroll;rx=(uint64_t)c->down[slot]*8/1000;tx=(uint64_t)c->up[slot]*8/1000;}
        }
        rx_x[i]=tx_x[i]=x;rx_y[i]=rx;tx_y[i]=tx;
    }
    lv_chart_set_range(a->obj,LV_CHART_AXIS_PRIMARY_Y,0,scale);
    if(valid && c->n>1)lv_obj_add_flag(a->empty,LV_OBJ_FLAG_HIDDEN);
    else {lv_obj_remove_flag(a->empty,LV_OBJ_FLAG_HIDDEN);text(a->empty,valid?"Collecting history":"No recent data");}
    lv_chart_refresh(a->obj);
}
void unifi_view_refresh(uint32_t now_ms,bool wifi,const char *local_ip){
    uint32_t now=now_ms/1000;
    unifi_sys_t s={0};unifi_extra_t e={0};unifi_curve_t c={0};unifi_ping_t ping={0};char b[100],r[32],t[32];
    bool live=unifi_get_sys(&s);bool extra=unifi_get_extra(&e);
    bool inv=extra && fresh(now,e.device_ts),clients=extra && e.online_valid && fresh(now,e.clients_ts);
    bool online=inv && e.device_valid && e.device_online;
    text(status,live?"Connected":"No data");tint(status,live?GREEN:AMBER);
    text(hero_name,inv && e.selected_name[0]?e.selected_name:"UniFi Console");
    text(hero_model,inv && e.selected_model[0]?e.selected_model:"Gateway");
    text(hero_state,inv?(online?"Online":"Not online"):"Unknown");tint(hero_state,online?GREEN:MUTED);
    if(inv)snprintf(b,sizeof(b),"%lu",(unsigned long)e.device_count);else strcpy(b,"--");text(counts[0],b);
    if(clients)snprintf(b,sizeof(b),"%lu",(unsigned long)e.online_count);else strcpy(b,"--");text(counts[1],b);
    if(inv)snprintf(b,sizeof(b),"%lu adopted",(unsigned long)e.device_count);else strcpy(b,"-- adopted");text(device_count,b);
    if(clients)snprintf(b,sizeof(b),"%lu total",(unsigned long)e.online_count);else strcpy(b,"-- total");text(client_count,b);
    visible(device_empty,!inv || !e.device_rows);text(device_empty,inv?"No adopted devices":"Device data unavailable");
    visible(client_empty,!clients || !e.client_cnt);text(client_empty,clients?"No connected clients":"Client data unavailable");
    static int art_kind[3]={-1,-1,-1};
    for(int i=0;i<3;i++){
        bool row=inv && i<e.device_rows;unifi_device_t *d=&e.devices[i];
        visible(device_shape[i],row);visible(device_name[i],row);visible(device_model[i],row);visible(device_state[i],row);
        text(device_name[i],row?d->name:"--");text(device_model[i],row?d->model:"");
        bool up=row && !strcmp(d->state,"ONLINE");text(device_state[i],row?(up?"Online":"Not online"):"");tint(device_state[i],up?GREEN:MUTED);
        int kind=row?(d->access_point?1:0):-1;
        if(art_kind[i]!=kind){lv_obj_clean(device_shape[i]);if(kind>=0)device_art(device_shape[i],1,1,kind==1);art_kind[i]=kind;}
        bool client=clients && i<e.client_cnt;unifi_client_t *cl=&e.client[i];
        visible(client_shape[i],client);visible(client_name[i],client);visible(client_ip[i],client);visible(client_type[i],client);
        text(client_name[i],client?cl->name:"--");text(client_ip[i],client?cl->ip:"");
        text(client_type[i],!client?"":!strcmp(cl->interface,"WIRELESS")?"WiFi":!strcmp(cl->interface,"WIRED")?"Wired":"VPN");
    }
    bool rate=live && s.rate_valid;
    if(rate){format_rate(s.down_bps,r,sizeof(r));format_rate(s.up_bps,t,sizeof(t));}else {strcpy(r,"--");strcpy(t,"--");}
    text(rates[0],r);text(rates[1],t);snprintf(b,sizeof(b),"RX  %s",r);text(overview_rates[0],b);snprintf(b,sizeof(b),"TX  %s",t);text(overview_rates[1],b);
    bool history=rate && unifi_get_curve(&c);
    if(c.ts!=last_chart_ts || history!=last_chart_valid){
        uint32_t peak=1000;
        for(int i=0;i<c.n;i++){
            uint32_t stamp=c.sample_ts[i];
            if(stamp>now || now-stamp>WINDOW_SEC)continue;
            uint32_t v=(uint64_t)(c.down[i]>c.up[i]?c.down[i]:c.up[i])*8/1000;
            if(v>peak)peak=v;
        }
        // Leave headroom; grow immediately to avoid clipping a new peak.
        // Shrink only below half scale, then ease down rather than moving all
        // historical vertices abruptly when a peak leaves the visible window.
        int32_t desired=(int32_t)(ceil(peak*1.2/1000.0)*1000);
        if(!last_chart_valid || !history || desired>animation_scale)
            animation_scale=scale_target=desired;
        else if(desired<=animation_scale/2 || desired>scale_target)scale_target=desired;
        animation_curve=c;scale_tick_ms=now_ms;
        render_activity(&mini,&c,now_ms,animation_scale,history);render_activity(&traffic,&c,now_ms,animation_scale,history);
        if(history)snprintf(b,sizeof(b),"%.1f Mbps",animation_scale/1000.0);else strcpy(b,"-- Mbps");text(traffic_scale,b);
        last_chart_ts=c.ts;last_chart_valid=history;
    }
    text(system_name,inv?e.selected_model:"Console");snprintf(b,sizeof(b),"Firmware %s",inv && e.version_valid?e.version:"--");text(system_version,b);
    if(live && s.cpu_valid)snprintf(b,sizeof(b),"%.0f%%",(double)s.cpu_pct);else strcpy(b,"--");text(cpu,b);
    if(live && s.mem_valid)snprintf(b,sizeof(b),"%.0f%%",(double)s.mem_pct);else strcpy(b,"--");text(memory,b);
    lv_obj_set_width(bars[0],live && s.cpu_valid?(int)(s.cpu_pct*1.26f):0);lv_obj_set_width(bars[1],live && s.mem_valid?(int)(s.mem_pct*1.26f):0);
    if(live && e.uptime_valid)snprintf(b,sizeof(b),"%lud %luh %lum",(unsigned long)(e.uptime_sec/86400),(unsigned long)(e.uptime_sec/3600%24),(unsigned long)(e.uptime_sec/60%60));else strcpy(b,"--");text(uptime,b);
    if(live && e.load_valid[0] && e.load_valid[1] && e.load_valid[2])snprintf(b,sizeof(b),"%.2f / %.2f / %.2f",(double)e.load_avg[0],(double)e.load_avg[1],(double)e.load_avg[2]);else strcpy(b,"-- / -- / --");text(load,b);
    text(system_ip,"");
    if(!live)snprintf(b,sizeof(b),"%s",failure(wifi));
    else if(selected==1){if(inv)snprintf(b,sizeof(b),"%d shown / %lu adopted",e.device_rows,(unsigned long)e.device_count);else strcpy(b,"Device inventory unavailable");}
    else if(selected==2){if(clients)snprintf(b,sizeof(b),"%d shown / %lu connected",e.client_cnt,(unsigned long)e.online_count);else strcpy(b,"Client list unavailable");}
    else if(selected==3)snprintf(b,sizeof(b),"Uplink RX / TX  |  Live");
    else if(selected==4)snprintf(b,sizeof(b),"Console  %s",inv?e.selected_ip:"--");
    else if(unifi_get_ping(&ping)){int i=(ping.head+UNIFI_PING_MAX-1)%UNIFI_PING_MAX;if(ping.ms[i]<0)strcpy(b,"Local gateway / no reply");else snprintf(b,sizeof(b),"Local gateway  %.0f ms",(double)ping.ms[i]);}
    else snprintf(b,sizeof(b),"Device uplink  |  Live");
    (void)local_ip;
    text(footer,b);
}
void unifi_view_step(int direction){select_view((selected+direction+VIEWS)%VIEWS);}
void unifi_view_home(void){select_view(0);}
int unifi_view_page(void){return selected;}

// Animate time position and axis shrink; values remain actual API samples.
void unifi_view_animate(uint32_t now_ms){
    uint32_t dt=now_ms>=scale_tick_ms?now_ms-scale_tick_ms:0;
    scale_tick_ms=now_ms;
    if(last_chart_valid && animation_scale>scale_target && dt){
        if(dt>1000)dt=1000;
        int32_t step=(int64_t)(animation_scale-scale_target)*dt/2000;
        animation_scale-=step?step:1;
        char b[32];snprintf(b,sizeof(b),"%.1f Mbps",animation_scale/1000.0);text(traffic_scale,b);
    }
    if(last_chart_valid && (selected==0 || selected==3))
        render_activity(selected==0?&mini:&traffic,&animation_curve,now_ms,animation_scale,true);
}
