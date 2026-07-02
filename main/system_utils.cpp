// main/system_utils.cpp
#include "system_utils.h"
#include "config.h"
#include "wifi_manager.h"
#include "ir_uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "UTILS";

void delayed_reboot_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void boot_button_task(void *pvParameter) {
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);
    
    ESP_LOGI(TAG, "Boot button listener started on GPIO %d", BOOT_BUTTON_PIN);
    
    while (1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                if (!s_ap_fallback_active) {
                    ESP_LOGW(TAG, "Boot button pressed! Forcing AP fallback mode...");
                    s_ap_fallback_active = true;
                    start_ap_server();
                }
                vTaskDelete(NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}