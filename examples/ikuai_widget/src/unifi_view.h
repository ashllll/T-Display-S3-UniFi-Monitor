#pragma once
#include <stdbool.h>
#include <stdint.h>
void unifi_view_create(void);
void unifi_view_refresh(uint32_t now_ms, bool wifi_online, const char *local_ip);
void unifi_view_step(int direction);
void unifi_view_home(void);
int unifi_view_page(void);

void unifi_view_animate(uint32_t now_ms);
