#include "dht_sensor.h"
#include "config.h"
#include "automation.h"
#include "ir_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DHT";
float current_temp = 0.0;
float current_hum = 0.0;
static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

int wait_for_level(int level, int timeout_us) {
    int count = 0;
    while (gpio_get_level(DHT_PIN) == level) {
        if (count++ > timeout_us) return -1;
        esp_rom_delay_us(1);
    }
    return count;
}

bool read_dht11(float *temp, float *hum) {
    uint8_t data[5] = {0, 0, 0, 0, 0};
    
    gpio_set_direction(DHT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); 
    gpio_set_level(DHT_PIN, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT_PIN, GPIO_MODE_INPUT);
    
    portENTER_CRITICAL(&dht_mux);
    if (wait_for_level(1, 100) < 0) { portEXIT_CRITICAL(&dht_mux); return false; }
    if (wait_for_level(0, 100) < 0) { portEXIT_CRITICAL(&dht_mux); return false; }
    if (wait_for_level(1, 100) < 0) { portEXIT_CRITICAL(&dht_mux); return false; }
    
    for (int i = 0; i < 40; i++) {
        if (wait_for_level(0, 100) < 0) { portEXIT_CRITICAL(&dht_mux); return false; }
        int t = wait_for_level(1, 100);
        if (t < 0) { portEXIT_CRITICAL(&dht_mux); return false; }
        
        data[i / 8] <<= 1;
        if (t > 40) data[i / 8] |= 1;
    }
    portENTER_CRITICAL(&dht_mux);
    
    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        *hum = data[0] + (data[1] * 0.1);
        *temp = data[2] + (data[3] * 0.1);
        return true;
    }
    return false;
}

void dht_task(void *pvParameter) {
    gpio_set_pull_mode(DHT_PIN, GPIO_PULLUP_ONLY);
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        float temp, hum;
        if (read_dht11(&temp, &hum)) {
            current_temp = temp;
            current_hum = hum;
            ESP_LOGI(TAG, "Read Success! Temp: %.1f C, Hum: %.1f %%, Active Automations: %d", current_temp, current_hum, num_autos);
            
            // Handle State-Enforcement Level Triggers
            if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
                for (int i = 0; i < num_autos; i++) {
                    bool condition_met = false;
                    
                    if (auto_rules[i].condition == 1 && current_temp >= auto_rules[i].threshold) condition_met = true;
                    if (auto_rules[i].condition == 0 && current_temp <= auto_rules[i].threshold) condition_met = true;
                    
                    if (condition_met) {
                        const char* target_cmd = get_command_string(auto_rules[i].command);
                        
                        // If the target state does not match the last command executed, enforce it
                        if (strcmp(last_command_str, target_cmd) != 0) {
                            ESP_LOGW(TAG, "Automation %d matched! Temp %.1f meets rule threshold %.1f. Enforcing state '%s' (Previous was '%s')", 
                                     i, current_temp, auto_rules[i].threshold, target_cmd, last_command_str);
                            execute_command(auto_rules[i].command);
                        }
                    }
                }
                xSemaphoreGive(auto_mutex);
            }
        } else {
            ESP_LOGW(TAG, "Read Failed! Check wires or verify pull-up resistor on GPIO %d.", DHT_PIN);
        }
    }
}