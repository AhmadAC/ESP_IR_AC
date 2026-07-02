#include "automation.h"
#include "ir_uart.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <time.h>
#include <string.h>

static const char *TAG = "AUTO";

TimerEntry timers[MAX_TIMERS];
int num_timers = 0;
SemaphoreHandle_t timer_mutex = NULL;

AutoEntry auto_rules[MAX_AUTOS];
bool auto_triggered[MAX_AUTOS];
int num_autos = 0;
SemaphoreHandle_t auto_mutex = NULL;

void init_automation() {
    timer_mutex = xSemaphoreCreateMutex();
    auto_mutex = xSemaphoreCreateMutex();
}

void load_timers() {
    nvs_handle_t my_handle;
    if(nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        uint32_t count = 0;
        if(nvs_get_u32(my_handle, "num_timers", &count) == ESP_OK) {
            if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
                num_timers = count;
                if(num_timers > MAX_TIMERS) num_timers = MAX_TIMERS;
                size_t required_size = sizeof(TimerEntry) * num_timers;
                if(required_size > 0) {
                    nvs_get_blob(my_handle, "timers", timers, &required_size);
                }
                xSemaphoreGive(timer_mutex);
            }
        }
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded %d timers from NVS", num_timers);
    }
}

void load_auto_config() {
    nvs_handle_t my_handle;
    if(nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        uint32_t count = 0;
        if(nvs_get_u32(my_handle, "num_autos", &count) == ESP_OK) {
            if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
                num_autos = count;
                if(num_autos > MAX_AUTOS) num_autos = MAX_AUTOS;
                size_t required_size = sizeof(AutoEntry) * num_autos;
                if(required_size > 0) {
                    nvs_get_blob(my_handle, "auto_rules", auto_rules, &required_size);
                }
                memset(auto_triggered, 0, sizeof(auto_triggered)); // Reset trigger states on boot
                xSemaphoreGive(auto_mutex);
            }
        }
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded %d automation rules from NVS", num_autos);
    }
}

void timer_task(void *pvParameter) {
    int last_executed_min = -1;
    while(1) {
        time_t now = 0;
        time(&now);
        struct tm timeinfo;
        memset(&timeinfo, 0, sizeof(timeinfo));
        localtime_r(&now, &timeinfo);
        
        if (timeinfo.tm_year > (2020 - 1900)) { // Valid time acquired
            if (timeinfo.tm_min != last_executed_min) {
                last_executed_min = timeinfo.tm_min;
                
                if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
                    for(int i = 0; i < num_timers; i++) {
                        if (timers[i].hour == timeinfo.tm_hour &&
                            timers[i].minute == timeinfo.tm_min &&
                            (timers[i].day == -1 || timers[i].day == timeinfo.tm_wday)) {
                            
                            ESP_LOGI(TAG, "Executing timer %d, cmd: %d", i, timers[i].command);
                            execute_command(timers[i].command);
                        }
                    }
                    xSemaphoreGive(timer_mutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}