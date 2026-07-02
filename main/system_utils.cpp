#include "system_utils.h"
#include "config.h"
#include "wifi_manager.h"
#include "ir_uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static const char *TAG = "UTILS";

void delayed_reboot_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void boot_button_task(void *pvParameter) {
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);
    while (1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                if (!s_ap_fallback_active) {
                    s_ap_fallback_active = true;
                    start_ap_server();
                }
                vTaskDelete(NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void console_read_task(void *pvParameter) {
    ESP_LOGI(TAG, "Native REPL Console Listener Task started.");
    
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    uint8_t buf[64];
    char cmd[64];
    int cmd_idx = 0;
    
    while (1) {
        int len = read(fd, buf, sizeof(buf));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (buf[i] == 0x03) { 
                    execute_command(0); 
                } else if (buf[i] == 0x04) {
                    esp_restart();
                } else if (buf[i] == '\r' || buf[i] == '\n') {
                    if (cmd_idx > 0) {
                        cmd[cmd_idx] = '\0';
                        if (strcmp(cmd, "reset") == 0) {
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_all(h);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            esp_restart();
                        } else if (strcmp(cmd, "ap") == 0) {
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "force_ap", 1);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            esp_restart();
                        } else if (strcmp(cmd, "wifi") == 0) {
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "force_ap", 0);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            esp_restart();
                        }
                        cmd_idx = 0;
                    }
                } else {
                    if (cmd_idx < sizeof(cmd) - 1) {
                        cmd[cmd_idx++] = (char)buf[i];
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}