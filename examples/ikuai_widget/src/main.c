// LILYGO T-Display-S3 — 桌面信息摆件（LVGL 界面）
// UniFi 设备状态、上联速率与趋势曲线
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "unifi_monitor.h"
#include "nvs_flash.h"
#include "lcd_driver.h"
#include "desktop_widget.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "=== UniFi RGB / smooth timeline === reset_reason=%d", esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Keep the backlight off until LVGL has rendered and transferred its
    // first complete frame.  Otherwise the panel can show undefined GRAM
    // contents as a power-on garbage screen before the UI task starts.
    lcd_init();

    widget_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        unifi_sys_t state={0};bool live=unifi_get_sys(&state);
        ESP_LOGI(TAG,"UniFi uptime=%llu live=%d heap=%lu",
            (unsigned long long)(esp_timer_get_time()/1000000),live,(unsigned long)esp_get_free_heap_size());
    }
}
