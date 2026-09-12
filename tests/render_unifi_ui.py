#!/usr/bin/env python3
"""Compile the shipped UniFi view with native LVGL; verify navigation/data and render fixtures."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
APP=ROOT/'examples/ikuai_widget'
OUT=Path('/tmp/unifi-console-render');OUT.mkdir(exist_ok=True)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unifi_view.c"
static unifi_sys_t sys;
static unifi_extra_t extra;
static unifi_curve_t history;
static unifi_link_state_t link_state=UNIFI_LINK_WAIT;
bool unifi_get_sys(unifi_sys_t *v){*v=sys;return sys.ok;}
bool unifi_get_extra(unifi_extra_t *v){*v=extra;return extra.ok;}
bool unifi_get_curve(unifi_curve_t *v){*v=history;return history.n>0;}
bool unifi_get_ping(unifi_ping_t *v){memset(v,0,sizeof(*v));return false;}
unifi_link_state_t unifi_get_link_state(void){return link_state;}
static uint16_t frame[320*170];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){
 uint16_t *s=(uint16_t*)p;for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++)frame[y*320+x]=*s++;
 lv_display_flush_ready(d);
}
static void snapshot(const char *name){
 lv_refr_now(lv_display_get_default());FILE *f=fopen(name,"wb");assert(f);fprintf(f,"P6\n320 170\n255\n");
 for(int i=0;i<320*170;i++){uint16_t v=frame[i];fputc(((v>>11)&31)*255/31,f);fputc(((v>>5)&63)*255/63,f);fputc((v&31)*255/31,f);}fclose(f);
}
static void check_bounds(lv_obj_t *o){
 if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
 lv_area_t a;lv_obj_get_coords(o,&a);
 if(lv_obj_check_type(o,&lv_label_class) && strlen(lv_label_get_text(o))){assert(a.x1>=0 && a.x2<320 && a.y1>=0 && a.y2<170);}
 for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)check_bounds(lv_obj_get_child(o,i));
}
int main(void){
 lv_init();lv_display_t *d=lv_display_create(320,170);static uint16_t buffer[320*170];lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(d,flush);
 unifi_view_create();unifi_view_refresh(200000,false,"--");snapshot("00-startup.ppm");
 assert(!strcmp(lv_label_get_text(footer),"Waiting for Wi-Fi"));
 sys=(unifi_sys_t){.ok=true,.rate_valid=true,.cpu_valid=true,.mem_valid=true,.cpu_pct=11.9,.mem_pct=66.2,.down_bps=479165,.up_bps=187036};
 extra=(unifi_extra_t){.ok=true,.device_valid=true,.device_online=true,.device_ts=200,.device_count=2,.device_rows=2,
 .online_valid=true,.online_count=23,.clients_ts=200,.client_cnt=3,.system_ts=200,.uptime_valid=true,.uptime_sec=95016,
 .version_valid=true,.load_valid={true,true,true},.load_avg={3.50,3.37,3.23}};
 strcpy(extra.selected_name,"Cloud Gateway Max");strcpy(extra.selected_model,"UCG Max");strcpy(extra.selected_ip,"192.0.2.1");strcpy(extra.version,"5.1.33");
 strcpy(extra.devices[0].name,"Cloud Gateway Max");strcpy(extra.devices[0].model,"UCG Max");strcpy(extra.devices[0].state,"ONLINE");
 strcpy(extra.devices[1].name,"AC Pro");strcpy(extra.devices[1].model,"AC Pro");strcpy(extra.devices[1].state,"ONLINE");extra.devices[1].access_point=true;
 const char *names[]={"MacBook Pro","Living room TV","Workstation"};
 for(int i=0;i<3;i++){strcpy(extra.client[i].name,names[i]);snprintf(extra.client[i].ip,sizeof(extra.client[i].ip),"192.0.2.%d",i+4);strcpy(extra.client[i].interface,i?"WIRED":"WIRELESS");}
 history.n=60;history.head=0;history.ts=200;
 for(int i=0;i<60;i++){history.sample_ts[i]=82+i*2;history.down[i]=(uint32_t)(350000+180000*sinf(i*.19)+75000*cosf(i*.71));history.up[i]=(uint32_t)(130000+60000*cosf(i*.26)+30000*sinf(i*.66));}
 for(int page=0;page<VIEWS;page++){
  unifi_view_refresh(200000,true,"192.0.2.9");lv_obj_update_layout(lv_screen_active());check_bounds(lv_screen_active());
  assert(unifi_view_page()==page);int visible=0;for(int j=0;j<VIEWS;j++)visible+=!lv_obj_has_flag(pages[j],LV_OBJ_FLAG_HIDDEN);assert(visible==1);
  char file[32];snprintf(file,sizeof(file),"%02d-%s.ppm",page+1,titles[page]);snapshot(file);unifi_view_step(1);
 }
 assert(unifi_view_page()==0);unifi_view_step(-1);assert(unifi_view_page()==4);unifi_view_home();
 assert(!strcmp(lv_label_get_text(client_count),"23 total"));assert(!strcmp(lv_label_get_text(cpu),"12%"));
 sys.down_bps=sys.up_bps=0;sys.cpu_pct=sys.mem_pct=100;unifi_view_refresh(200000,true,"192.0.2.9");assert(!strcmp(lv_label_get_text(rates[0]),"0 kbps"));
 unifi_view_step(-1);snapshot("06-full-load.ppm");
 sys.ok=false;link_state=UNIFI_LINK_AUTH_ERROR;unifi_view_home();unifi_view_refresh(240000,true,"192.0.2.9");snapshot("07-no-data.ppm");
 assert(!strcmp(lv_label_get_text(cpu),"--"));assert(!strcmp(lv_label_get_text(counts[0]),"--"));assert(!strcmp(lv_label_get_text(footer),"API key rejected"));
 assert(!lv_obj_has_flag(traffic.empty,LV_OBJ_FLAG_HIDDEN));
 assert(lv_chart_get_y_array(traffic.obj,traffic.rx)[0]==LV_CHART_POINT_NONE);
 char unit[32];format_rate(12500000,unit,sizeof(unit));assert(!strcmp(unit,"100.0 Mbps"));
 format_rate(125000000,unit,sizeof(unit));assert(!strcmp(unit,"1.00 Gbps"));
 // Horizontal coordinates use real time, and interruptions break the line.
 history.n=3;history.head=3;history.sample_ts[0]=170;history.sample_ts[1]=172;history.sample_ts[2]=199;
 sys.ok=true;unifi_view_refresh(200000,true,"192.0.2.9");
 assert(lv_chart_get_x_array(traffic.obj,traffic.rx)[0]==lv_obj_get_content_width(traffic.obj)+(170*lv_obj_get_content_width(traffic.obj)/120)-(200000LL*lv_obj_get_content_width(traffic.obj)/120000));
 unifi_view_home();unifi_view_animate(200500);
 assert(lv_chart_get_x_array(mini.obj,mini.rx)[0]==lv_obj_get_content_width(mini.obj)+(170*lv_obj_get_content_width(mini.obj)/120)-(200500LL*lv_obj_get_content_width(mini.obj)/120000));
 assert(lv_chart_get_y_array(traffic.obj,traffic.rx)[2]==LV_CHART_POINT_NONE);
 // New data in the middle of a second must not rewind the animated timeline.
 history.ts++;
 unifi_view_refresh(200600,true,"192.0.2.9");
 assert(lv_chart_get_x_array(mini.obj,mini.rx)[0]==lv_obj_get_content_width(mini.obj)+(170*lv_obj_get_content_width(mini.obj)/120)-(200600LL*lv_obj_get_content_width(mini.obj)/120000));
 unifi_view_animate(200650);
 assert(lv_chart_get_x_array(mini.obj,mini.rx)[0]==lv_obj_get_content_width(mini.obj)+(170*lv_obj_get_content_width(mini.obj)/120)-(200650LL*lv_obj_get_content_width(mini.obj)/120000));
 // Fixed segments retain pixel width in both chart sizes while scrolling.
 for(int chart_id=0;chart_id<2;chart_id++){
  unifi_view_home();if(chart_id)unifi_view_step(3);
  activity_t *a=chart_id?&traffic:&mini;
  unifi_view_animate(200650);
  lv_point_t p0,p1;
  lv_chart_get_point_pos_by_id(a->obj,a->rx,0,&p0);
  lv_chart_get_point_pos_by_id(a->obj,a->rx,1,&p1);
  int segment_width=p1.x-p0.x,last_x=p0.x;
  for(uint32_t tick=200700;tick<203000;tick+=50){
   unifi_view_animate(tick);
   lv_chart_get_point_pos_by_id(a->obj,a->rx,0,&p0);
   lv_chart_get_point_pos_by_id(a->obj,a->rx,1,&p1);
   assert(p1.x-p0.x==segment_width && p0.x<=last_x);
   last_x=p0.x;
  }
 }
 puts("PASS: actual UniFi view, five-view navigation, bounds, empty/zero/stale states and timestamp/gap chart mapping");
}
'''
(OUT/'render.c').write_text(code)
(OUT/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(unifi_console_preview C CXX)
set(LV_CONF_PATH "{APP}/components/lv_conf.h" CACHE STRING "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_subdirectory("{APP}/managed_components/lvgl__lvgl" lvgl)
add_executable(render render.c "{APP}/src/fonts/ui_font_lato_12.c" "{APP}/src/fonts/ui_font_lato_20.c")
target_include_directories(render PRIVATE "{APP}/src")
target_link_libraries(render PRIVATE lvgl m)
''')
for cmd in [['cmake','-S',str(OUT),'-B',str(OUT/'build')],['cmake','--build',str(OUT/'build'),'--target','render','-j','8'],[str(OUT/'build/render')]]:subprocess.run(cmd,cwd=OUT,check=True)
print(OUT)
