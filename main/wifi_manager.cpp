// main/wifi_manager.cpp
#include "wifi_manager.h"
#include "web_server.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static int s_retry_num = 0;
bool s_ap_fallback_active = false;
bool s_wifi_is_started = false; 
static TaskHandle_t dns_task_handle = NULL;

void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512]; 
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr)); // Crucial zero-init for socket binding
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS Server listening on port 53");

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        memset(&source_addr, 0, sizeof(source_addr));
        
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 32, 0, (struct sockaddr *)&source_addr, &socklen);
        
        if (len < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        
        // Very basic DNS responder to redirect captive portal queries to 192.168.4.1
        if (len > 12) {
            rx_buffer[2] = 0x81; 
            rx_buffer[3] = 0x80;
            rx_buffer[6] = rx_buffer[4]; 
            rx_buffer[7] = rx_buffer[5];
            rx_buffer[8] = 0; rx_buffer[9] = 0; 
            rx_buffer[10] = 0; rx_buffer[11] = 0; 
            
            int pos = len;
            rx_buffer[pos++] = 0xC0; rx_buffer[pos++] = 0x0C;
            rx_buffer[pos++] = 0x00; rx_buffer[pos++] = 0x01;
            rx_buffer[pos++] = 0x00; rx_buffer[pos++] = 0x01;
            rx_buffer[pos++] = 0x00; rx_buffer[pos++] = 0x00;
            rx_buffer[pos++] = 0x00; rx_buffer[pos++] = 0x3C;
            rx_buffer[pos++] = 0x00; rx_buffer[pos++] = 0x04;
            rx_buffer[pos++] = 192;  rx_buffer[pos++] = 168;
            rx_buffer[pos++] = 4;    rx_buffer[pos++] = 1;
            
            sendto(sock, rx_buffer, pos, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }
}

void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Time successfully synced with NTP.");
}

void initialize_sntp(void) {
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    setenv("TZ", "TZ=CST-8", 1);
    tzset();
}

static void ap_fallback_task(void *pvParameter) {
    start_ap_server();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_fallback_active) return;
        
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to local router (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            ESP_LOGW(TAG, "Connection attempts exhausted. Rolling back to AP Config Portal...");
            s_ap_fallback_active = true;
            xTaskCreate(ap_fallback_task, "ap_fallback", 4096, NULL, 5, NULL);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected to SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected from SoftAP, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        
        initialize_sntp();
        
        ESP_ERROR_CHECK(mdns_init());
        mdns_hostname_set("esp32-ac-ctrl");
        mdns_instance_name_set("ESP32 AC Controller");
        mdns_service_add("ESP32 AC Controller", "http", "tcp", 80, NULL, 0); 
        
        if (!server_disabled_by_timer) {
            start_web_server();
        }
    }
}

void start_ap_server() {
    ESP_LOGI(TAG, "Configuring and Launching SoftAP Portal...");
    
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "ESP32_AC_Config");
    strcpy((char*)wifi_config.ap.password, "12345678");
    wifi_config.ap.ssid_len = strlen("ESP32_AC_Config");
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (s_wifi_is_started) {
        esp_wifi_stop();
        s_wifi_is_started = false;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_is_started = true;
    ESP_LOGI(TAG, "SoftAP broadcast started! SSID: ESP32_AC_Config, IP: 192.168.4.1");
    
    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 5, &dns_task_handle);
    }
    start_web_server();
}

void wifi_init_and_connect() {
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    nvs_handle_t my_handle;
    bool has_credentials = false;
    uint8_t force_ap_u8 = 0;

    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u8(my_handle, "force_ap", &force_ap_u8);
        char ssid[32]; char pass[64];
        size_t s_len = sizeof(ssid); size_t p_len = sizeof(pass);
        
        if (force_ap_u8 == 0 && nvs_get_str(my_handle, "wifi_ssid", ssid, &s_len) == ESP_OK && nvs_get_str(my_handle, "wifi_pass", pass, &p_len) == ESP_OK) {
            ESP_LOGI(TAG, "Saved network credentials found. Connecting to: %s", ssid);
            wifi_config_t wifi_config = {};
            strcpy((char*)wifi_config.sta.ssid, ssid);
            strcpy((char*)wifi_config.sta.password, pass);
            
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());
            s_wifi_is_started = true;
            has_credentials = true;
        }
        nvs_close(my_handle);
    }

    if (!has_credentials || force_ap_u8 == 1) {
        s_ap_fallback_active = true;
        start_ap_server();
    }
}