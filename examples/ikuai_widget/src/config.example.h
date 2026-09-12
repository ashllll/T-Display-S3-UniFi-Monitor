#pragma once

// ── 配置模板：复制为 src/config.h 并填写真实值 ──────────────────────
// src/config.h 已在 .gitignore 中，不会进入版本库

// 1 = 离线演示数据，不连接 Wi-Fi/UniFi；0 = 使用下面的真实配置。
// 允许正式构建环境通过 -DAPP_DEMO_MODE 强制选择模式。
#ifndef APP_DEMO_MODE
#define APP_DEMO_MODE 1
#endif

// 历史兼容开关；当前界面无装饰粒子动效
#define APP_REDUCED_MOTION 1

#define APP_WIFI_SSID "YOUR_SSID"
#define APP_WIFI_PASS "YOUR_PASSWORD"

#define APP_TZ "CST-8"

#define APP_BL_PCT 45

// 夜间自动降背光(SNTP 授时后生效;手动息屏优先)
// 时段跨零点也可,例如 23~7 表示 23:00~次日 07:00
#define APP_NIGHT_START 23
#define APP_NIGHT_END   7
#define APP_BL_NIGHT_PCT 12

// UniFi API 配置见 unifi_config.example.h 和 unifi_cert.example.h。
