// main/main.cpp
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Custom Modules
#include "config.h"
#include "ir_uart.h"
#include "dht_sensor.h"
#include "automation.h"
#include "wifi_manager.h"
#include "system_utils.h"

// Dedicated task with a large stack to perform network initialization safely
void system_init_task(void *pvParameter) {
    wifi_init_and_connect();
    vTaskDelete(NULL); // Self-delete when configuration is complete
}

extern "C" void app_main(void) {
    // 1. Initialize Non-Volatile Storage First
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Core Initializations
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    init_uart();
    init_automation();

    // 3. Load logic states saved from previous boots
    load_timers();
    load_auto_config();

    // 4. Start isolated Background Tasks
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);
    xTaskCreate(dht_task, "dht_task", 4096, NULL, 5, NULL);
    xTaskCreate(boot_button_task, "button_task", 2048, NULL, 10, NULL);
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);

    // 5. Spawn stack-safe network setup task
    xTaskCreate(system_init_task, "sys_init_task", 8192, NULL, 5, NULL);
}