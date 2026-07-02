// main/main.cpp
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// Custom modules
#include "ir_codes.h"

#define UART_PORT_NUM UART_NUM_1
#define TXD_PIN (GPIO_NUM_17) // Wire to IR Module RXD
#define RXD_PIN (GPIO_NUM_18) // Wire to IR Module TXD
#define DHT_PIN (GPIO_NUM_4)  // Wire to DHT11 'S' (Signal) pin
#define BOOT_BUTTON_PIN GPIO_NUM_0 
#define MAXIMUM_RETRY 5

static const char *TAG = "AC_CTRL";
httpd_handle_t server = NULL;
static int s_retry_num = 0;
static bool s_ap_fallback_active = false;
char last_command_str[32] = "None";
bool server_disabled_by_timer = false;
static TaskHandle_t dns_task_handle = NULL;

// Timers variables
typedef struct {
    int day;      // -1 for everyday, 0=Sun .. 6=Sat
    int hour;     // 0-23
    int minute;   // 0-59
    int command;  // Maps directly to execution IDs 0 through 14
} TimerEntry;

#define MAX_TIMERS 20
TimerEntry timers[MAX_TIMERS];
int num_timers = 0;
SemaphoreHandle_t timer_mutex = NULL;

// Automation & Sensor Variables
float current_temp = 0.0;
float current_hum = 0.0;
static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

#define MAX_AUTOS 10
typedef struct {
    float threshold;
    int condition; // 0 = Below or equal, 1 = Above or equal
    int command;
} AutoEntry;

AutoEntry auto_rules[MAX_AUTOS];
bool auto_triggered[MAX_AUTOS]; // Keeps track if the rule has already executed to prevent spamming
int num_autos = 0;
SemaphoreHandle_t auto_mutex = NULL;

void start_web_server();
void start_ap_server();
void execute_command(int cmd);

/* ==============================================
   NVS DATA PERSISTENCE
   ============================================== */
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
                memset(auto_triggered, 0, sizeof(auto_triggered)); // Reset trigger states
                xSemaphoreGive(auto_mutex);
            }
        }
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded %d automation rules from NVS", num_autos);
    }
}

/* ==============================================
   DHT11 SENSOR POLLING
   ============================================== */
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
    vTaskDelay(pdMS_TO_TICKS(20)); // Wake up standard is 18ms-20ms low
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
    portEXIT_CRITICAL(&dht_mux);
    
    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        *hum = data[0] + (data[1] * 0.1);
        *temp = data[2] + (data[3] * 0.1);
        return true;
    }
    return false;
}

void dht_task(void *pvParameter) {
    gpio_set_pull_mode(DHT_PIN, GPIO_PULLUP_ONLY); // Provide internal pull-up fallback
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // Sample every 3 seconds
        
        float temp, hum;
        if (read_dht11(&temp, &hum)) {
            current_temp = temp;
            current_hum = hum;
            
            // Handle Automation Triggers
            if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
                for (int i = 0; i < num_autos; i++) {
                    bool condition_met = false;
                    
                    if (auto_rules[i].condition == 1 && current_temp >= auto_rules[i].threshold) condition_met = true;
                    if (auto_rules[i].condition == 0 && current_temp <= auto_rules[i].threshold) condition_met = true;
                    
                    if (condition_met && !auto_triggered[i]) {
                        ESP_LOGI(TAG, "Automation %d Triggered! Temp: %.1f, Thresh: %.1f, Cmd: %d", 
                                 i, current_temp, auto_rules[i].threshold, auto_rules[i].command);
                        execute_command(auto_rules[i].command);
                    }
                    
                    auto_triggered[i] = condition_met; // Record state so it doesn't loop forever
                }
                xSemaphoreGive(auto_mutex);
            }
        }
    }
}


/* ==============================================
   DNS CAPTIVE PORTAL TASK
   ============================================== */
void dns_server_task(void *pvParameters) {
    char rx_buffer[128];
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE("DNS", "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE("DNS", "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("DNS", "DNS Server listening on port 53");

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        
        if (len < 0) {
            ESP_LOGE("DNS", "recvfrom failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (len > 12) {
            rx_buffer[2] = 0x81; // Flags: Standard query response, No error
            rx_buffer[3] = 0x80;
            rx_buffer[6] = rx_buffer[4]; // Answer RRs = Question RRs
            rx_buffer[7] = rx_buffer[5];
            rx_buffer[8] = 0; rx_buffer[9] = 0; // Authority RRs
            rx_buffer[10] = 0; rx_buffer[11] = 0; // Additional RRs
            
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


/* ==============================================
   UART / IR MODULE COMMUNICATION
   ============================================== */
void init_uart() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART initialized to interface with IR Learning Module.");
}

void send_uart_ir(const uint8_t* data, size_t data_len) {
    size_t frame_len = data_len + 7;
    uint8_t* frame = (uint8_t*)malloc(frame_len);
    
    frame[0] = 0x68;                     // Frame Head
    frame[1] = frame_len & 0xFF;         // Length Low byte
    frame[2] = (frame_len >> 8) & 0xFF;  // Length High byte
    frame[3] = 0x00;                     // Module Address
    frame[4] = 0x22;                     // Function Code (22H = Send external stored encoding)

    uint16_t checksum = 0x00 + 0x22;
    for(size_t i = 0; i < data_len; i++) {
        frame[5 + i] = data[i];
        checksum += data[i];
    }

    frame[5 + data_len] = checksum % 256; // Modulo 256 Checksum
    frame[6 + data_len] = 0x16;           // Frame Tail

    uart_write_bytes(UART_PORT_NUM, (const char*)frame, frame_len);
    ESP_LOGI(TAG, "Sent IR Command via UART (Length: %zu)", frame_len);
    free(frame);
}

/* ==============================================
   COMMAND & TIMER EXECUTION
   ============================================== */
void execute_command(int cmd) {
    switch (cmd) {
        case 0:
            strcpy(last_command_str, "AC OFF");
            send_uart_ir(ir_off, ir_off_len);
            break;
        case 1:
            strcpy(last_command_str, "AC ON");
            send_uart_ir(ir_on, ir_on_len);
            break;
        case 2:
            strcpy(last_command_str, "Set 19C");
            send_uart_ir(ir_19c, ir_19c_len);
            break;
        case 3:
            strcpy(last_command_str, "Set 20C");
            send_uart_ir(ir_20c, ir_20c_len);
            break;
        case 4:
            strcpy(last_command_str, "Set 21C");
            send_uart_ir(ir_21c, ir_21c_len);
            break;
        case 5:
            strcpy(last_command_str, "Set 22C");
            send_uart_ir(ir_22c, ir_22c_len);
            break;
        case 6:
            strcpy(last_command_str, "Set 23C");
            send_uart_ir(ir_23c, ir_23c_len);
            break;
        case 7:
            strcpy(last_command_str, "Set 24C");
            send_uart_ir(ir_24c, ir_24c_len);
            break;
        case 8:
            strcpy(last_command_str, "Set 25C");
            send_uart_ir(ir_25c, ir_25c_len);
            break;
        case 9:
            strcpy(last_command_str, "Set 26C");
            send_uart_ir(ir_26c, ir_26c_len);
            break;
        case 10:
            strcpy(last_command_str, "Set 27C");
            send_uart_ir(ir_27c, ir_27c_len);
            break;
        case 11:
            strcpy(last_command_str, "Mode HOT");
            send_uart_ir(ir_hot, ir_hot_len);
            break;
        case 12:
            strcpy(last_command_str, "Mode COLD");
            send_uart_ir(ir_cold, ir_cold_len);
            break;
        case 13:
            strcpy(last_command_str, "Server OFF");
            server_disabled_by_timer = true;
            if (server != NULL) {
                ESP_LOGI(TAG, "Stopping Web Server due to Timer Command...");
                httpd_stop(server);
                server = NULL;
            }
            break;
        case 14:
            strcpy(last_command_str, "Server ON");
            server_disabled_by_timer = false;
            if (server == NULL) {
                ESP_LOGI(TAG, "Starting Web Server due to Timer Command...");
                start_web_server();
            }
            break;
        default:
            ESP_LOGW(TAG, "Unknown command ID: %d", cmd);
            break;
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
        vTaskDelay(pdMS_TO_TICKS(5000)); // Scan active clock thresholds frequently 
    }
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

void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Time successfully synced with NTP.");
}

void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP Engine...");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    
    // Set Timezone logic to UTC+8 (GMT+8)
    setenv("TZ", "TZ=CST-8", 1);
    tzset();
}

/* ==============================================
   HTML UI PAYLOAD
   ============================================== */
const char HTML_UI[] = R"raw_html(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>ESP32 A/C Controller</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
    :root { --primary: #0ea5e9; --bg: #0f172a; --card: #1e293b; --text: #f1f5f9; }
    body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 15px; display: flex; flex-direction: column; align-items: center; margin:0; }
    .container { width: 100%; max-width: 500px; }
    .card { background: var(--card); padding: 25px; border-radius: 20px; border: 1px solid #334155; text-align: center; margin-bottom: 20px; }
    h2 { color: var(--primary); margin-top: 0; }
    input, select { width: 100%; padding: 12px; margin: 8px 0 20px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: white; box-sizing: border-box; }
    button { width: 100%; padding: 15px; border: none; border-radius: 12px; font-weight: bold; cursor: pointer; color: white; margin-top:10px; transition: transform 0.1s; }
    button:active { transform: scale(0.96); opacity: 0.9; }
    .btn-green { background: #10b981; }
    .btn-red { background: #ef4444; }
    .btn-blue { background: #3b82f6; }
    .btn-gray { background: #475569; }
    .btn-add { background: #0ea5e9; padding: 10px; border-radius: 8px; width: auto; font-size: 0.9rem; margin-top: 5px; }
    .status-bar { padding: 12px; border-radius: 10px; font-weight: bold; text-align: center; font-size: 0.85rem; border: 1px solid; text-transform: uppercase; background: #172554; color: #93c5fd; border-color: #3b82f6; margin-bottom:15px; }
    .text-sm { font-size: 0.85rem; color: #94a3b8; margin-bottom:10px; }
    .sensor-val { font-size: 1.5rem; color: #38bdf8; font-weight: bold; }
    table { border-collapse: collapse; width: 100%; margin-bottom: 20px; font-size: 0.9rem; }
    th, td { border: 1px solid #475569; padding: 10px; text-align: center; }
    th { background: #0f172a; color: #0ea5e9; }
    .del-btn { background: #ef4444; padding: 5px; margin: 0; border-radius: 6px; }
    .flex-row { display: flex; gap: 10px; align-items: center; justify-content: center;}
</style>
</head>
<body>
    <div class="container">
        <div class="status-bar">A/C CONTROLLER DASHBOARD</div>
        
        <!-- Sensor Data Card -->
        <div class="card" style="display:flex; justify-content:space-around;">
            <div>
                <div class="text-sm">Temperature</div>
                <div class="sensor-val"><span id="curtemp">--</span> &deg;C</div>
            </div>
            <div>
                <div class="text-sm">Humidity</div>
                <div class="sensor-val"><span id="curhum">--</span> %</div>
            </div>
        </div>

        <!-- Controls Card -->
        <div class="card">
            <h2>Control A/C</h2>
            <div class="text-sm"><b>Time:</b> <span id="curtime">Loading...</span></div>
            <div class="text-sm"><b>Last Command:</b> <span id="lastcmd" style="color:#3b82f6;">None</span></div>
            
            <div style="display:flex; gap:10px;">
                <button class="btn-gray" onclick="c(0)">Turn OFF</button>
                <button class="btn-green" onclick="c(1)">Turn ON</button>
            </div>
            
            <div style="display:grid; grid-template-columns: repeat(3, 1fr); gap:8px; margin-top:10px;">
                <button class="btn-blue" onclick="c(2)">19&deg;C</button>
                <button class="btn-blue" onclick="c(3)">20&deg;C</button>
                <button class="btn-blue" onclick="c(4)">21&deg;C</button>
                <button class="btn-blue" onclick="c(5)">22&deg;C</button>
                <button class="btn-blue" onclick="c(6)">23&deg;C</button>
                <button class="btn-blue" onclick="c(7)">24&deg;C</button>
                <button class="btn-blue" onclick="c(8)">25&deg;C</button>
                <button class="btn-blue" onclick="c(9)">26&deg;C</button>
                <button class="btn-blue" onclick="c(10)">27&deg;C</button>
            </div>
            
            <div style="display:flex; gap:10px; margin-top:8px;">
                <button class="btn-red" onclick="c(11)">HOT Mode</button>
                <button class="btn-blue" style="background:#0ea5e9;" onclick="c(12)">COLD Mode</button>
            </div>
            <p id="cstat" class="text-sm" style="margin-top:15px;"></p>
        </div>
        
        <!-- Temp Automation Card -->
        <div class="card">
            <h2>Auto-Trigger Actions</h2>
            <div class="text-sm">Trigger IR commands based on temperature.</div>
            
            <table id="autoTbl">
                <thead><tr><th>Condition</th><th>Threshold</th><th>Action</th><th></th></tr></thead>
                <tbody id="autoBody"></tbody>
            </table>

            <div class="flex-row" style="margin-top: 15px;">
                <select id="a_cond" style="margin:0; flex:1;">
                    <option value="1">Temp &ge;</option>
                    <option value="0">Temp &le;</option>
                </select>
                <input type="number" id="a_thresh" step="0.5" style="margin:0; flex:1;" placeholder="25.0">
                <div class="text-sm" style="margin:0;">&deg;C</div>
            </div>
            
            <div class="flex-row" style="margin-top: 15px;">
                <select id="a_cmd" style="margin:0; flex:1;">
                    <option value="0">AC OFF</option>
                    <option value="1">AC ON</option>
                    <option value="2">Set 19C</option>
                    <option value="3">Set 20C</option>
                    <option value="4">Set 21C</option>
                    <option value="5">Set 22C</option>
                    <option value="6">Set 23C</option>
                    <option value="7">Set 24C</option>
                    <option value="8">Set 25C</option>
                    <option value="9">Set 26C</option>
                    <option value="10">Set 27C</option>
                    <option value="11">Mode HOT</option>
                    <option value="12">Mode COLD</option>
                </select>
                <button class="btn-add" onclick="addAuto()">Add +</button>
            </div>
            
            <button class="btn-green" style="margin-top:20px;" onclick="saveAutos()">Save Automations to Device</button>
            <p id="astat" class="text-sm" style="margin-top:10px;"></p>
        </div>

        <!-- Timers Card -->
        <div class="card">
            <h2>Timers</h2>
            <table id="timersTbl">
                <thead><tr><th>Day</th><th>Time</th><th>Action</th><th></th></tr></thead>
                <tbody id="timersBody"></tbody>
            </table>
            
            <select id="t_day">
                <option value="-1">Everyday</option><option value="0">Sunday</option><option value="1">Monday</option>
                <option value="2">Tuesday</option><option value="3">Wednesday</option><option value="4">Thursday</option>
                <option value="5">Friday</option><option value="6">Saturday</option>
            </select>
            <input type="time" id="t_time">
            
            <div class="flex-row">
                <select id="t_cmd" style="margin:0; flex:1;">
                    <option value="0">AC OFF</option>
                    <option value="1">AC ON</option>
                    <option value="2">Set 19C</option>
                    <option value="3">Set 20C</option>
                    <option value="4">Set 21C</option>
                    <option value="5">Set 22C</option>
                    <option value="6">Set 23C</option>
                    <option value="7">Set 24C</option>
                    <option value="8">Set 25C</option>
                    <option value="9">Set 26C</option>
                    <option value="10">Set 27C</option>
                    <option value="11">Mode HOT</option>
                    <option value="12">Mode COLD</option>
                    <option value="13">Server OFF</option>
                    <option value="14">Server ON</option>
                </select>
                <button class="btn-add" onclick="addTimer()">Add +</button>
            </div>
            
            <button class="btn-green" style="margin-top:20px;" onclick="saveTimers()">Save Timers to Device</button>
            <p id="tstat" class="text-sm" style="margin-top:10px;"></p>
        </div>

        <!-- Wi-Fi Setup Card -->
        <div class="card">
            <h2>Wi-Fi Settings</h2>
            <div id="status" class="text-sm">Ready to Scan</div>
            <button class="btn-gray" onclick="scan()">Scan Networks</button>
            
            <select id="ssid" style="margin-top:15px;"><option value="">-- Select --</option></select>
            <input type="password" id="pass" placeholder="Password">
            
            <button class="btn-green" onclick="save()">Save and Connect</button>
            
            <hr style="border-color:#334155; margin: 25px 0;">
            <div class="text-sm">Quick Boot Mode Switch</div>
            <button style="background:#f59e0b;" onclick="forceAP()">Force AP Mode</button>
            <button style="background:#10b981;" onclick="forceWiFi()">Use Saved Wi-Fi</button>
        </div>
    </div>

    <script>
        let timers = [];
        let autos = [];
        const days = {'-1':'Everyday','0':'Sun','1':'Mon','2':'Tue','3':'Wed','4':'Thu','5':'Fri','6':'Sat'};
        const cmds = {'0':'OFF','1':'ON','2':'19C','3':'20C','4':'21C','5':'22C','6':'23C','7':'24C','8':'25C','9':'26C','10':'27C','11':'HOT','12':'COLD','13':'SRV OFF','14':'SRV ON'};
        const conds = {'0':'&le;', '1':'&ge;'};
        
        function c(cmd){
            document.getElementById('cstat').innerText = 'Sending...';
            fetch('/ir?cmd='+cmd).then(r=>r.text()).then(t=>{
                document.getElementById('cstat').innerText = 'Status: ' + t;
                setTimeout(loadStatus, 600);
            });
        }
        
        // ---- AUTOMATIONS LOGIC ----
        function renderAutos(){
            let b = document.getElementById('autoBody');
            b.innerHTML = '';
            autos.forEach((a, i)=>{
                let tr = document.createElement('tr');
                tr.innerHTML = `<td>Temp ${conds[a.c]}</td><td>${a.t}&deg;C</td><td>${cmds[a.cmd]}</td><td><button class="del-btn" onclick="delAuto(${i})">X</button></td>`;
                b.appendChild(tr);
            });
        }

        function addAuto(){
            let cStr = document.getElementById('a_cond').value;
            let tStr = document.getElementById('a_thresh').value;
            let cmdStr = document.getElementById('a_cmd').value;
            if(!tStr) return alert('Enter a valid temperature threshold.');
            
            autos.push({ c: parseInt(cStr), t: parseFloat(tStr), cmd: parseInt(cmdStr) });
            renderAutos();
        }

        function delAuto(i){ autos.splice(i, 1); renderAutos(); }

        function saveAutos(){
            document.getElementById('astat').innerText = 'Saving...';
            fetch('/auto', {method:'POST', body:JSON.stringify(autos)}).then(()=>{
                document.getElementById('astat').innerText = 'Saved!';
                setTimeout(() => document.getElementById('astat').innerText='', 2000);
            });
        }

        // ---- TIMERS LOGIC ----
        function renderTimers(){
            let b = document.getElementById('timersBody');
            b.innerHTML = '';
            timers.forEach((t, i)=>{
                let tr = document.createElement('tr');
                tr.innerHTML = `<td>${days[t.d]}</td><td>${t.h.toString().padStart(2,'0')}:${t.m.toString().padStart(2,'0')}</td><td>${cmds[t.c]}</td><td><button class="del-btn" onclick="delTimer(${i})">X</button></td>`;
                b.appendChild(tr);
            });
        }

        function addTimer(){
            let d = parseInt(document.getElementById('t_day').value);
            let timeStr = document.getElementById('t_time').value;
            let c = parseInt(document.getElementById('t_cmd').value);
            if(!timeStr) return alert('Select a valid time.');
            let parts = timeStr.split(':');
            timers.push({ d:d, h:parseInt(parts[0]), m:parseInt(parts[1]), c:c });
            renderTimers();
        }

        function delTimer(i){ timers.splice(i, 1); renderTimers(); }

        function saveTimers(){
            document.getElementById('tstat').innerText = 'Saving...';
            fetch('/timers', { method:'POST', body:JSON.stringify(timers) }).then(()=>{
                document.getElementById('tstat').innerText = 'Saved!';
                setTimeout(() => document.getElementById('tstat').innerText='', 2000);
            });
        }

        // ---- SYSTEM LOGIC ----
        function scan(){
            document.getElementById('status').innerText = 'Scanning...';
            fetch('/scan').then(r=>r.json()).then(d=>{
                let s = document.getElementById('ssid');
                s.innerHTML = '<option value="">-- Select --</option>';
                d.forEach(n=>{ s.innerHTML += '<option value="'+n+'">'+n+'</option>'; });
                document.getElementById('status').innerText = 'Found ' + d.length + ' networks';
            }).catch(() => document.getElementById('status').innerText = 'Scan Error');
        }
        
        function save(){
            let s = document.getElementById('ssid').value, p = document.getElementById('pass').value;
            if(!s) return alert('Select SSID');
            fetch('/save', { method:'POST', body:JSON.stringify({ssid:s, pass:p}) }).then(()=>{
                alert('Credentials saved! Rebooting...');
            });
        }
        
        function forceAP(){
            if(confirm("Switch to AP Mode and reboot?")) fetch('/switch_to_ap', { method: 'POST' }).then(() => alert('Rebooting...'));
        }
        
        function forceWiFi(){
            if(confirm("Switch to Wi-Fi Mode and reboot?")) fetch('/switch_to_wifi', { method: 'POST' }).then(() => alert('Rebooting...'));
        }

        let isInitialLoad = true;
        function loadStatus(){
            fetch('/status').then(r=>r.json()).then(d=>{
                document.getElementById('curtime').innerText = d.time;
                document.getElementById('lastcmd').innerText = d.last_cmd;
                document.getElementById('curtemp').innerText = d.temp.toFixed(1);
                document.getElementById('curhum').innerText = d.hum.toFixed(1);
                
                if (isInitialLoad) {
                    timers = d.timers || [];
                    autos = d.autos || [];
                    renderTimers();
                    renderAutos();
                    isInitialLoad = false;
                }
            });
        }

        setInterval(loadStatus, 5000);
        window.onload = loadStatus;
    </script>
</body></html>
)raw_html";

/* ==============================================
   HTTP SERVER (CONTROL & PROVISIONING)
   ============================================== */
void delayed_reboot_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_UI, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_portal_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ir_get_handler(httpd_req_t *req) {
    char buf[50];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "cmd", param, sizeof(param)) == ESP_OK) {
            int cmd = atoi(param);
            ESP_LOGI(TAG, "Received IR Web Command: %d", cmd);
            
            // Only allow IR triggers (0 through 12) from web directly to avoid race conditions with server manipulation tasks
            if (cmd >= 0 && cmd <= 12) {
                execute_command(cmd);
                httpd_resp_sendstr(req, "Command Sent");
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Direct webserver on/off modification unsupported via GET IR.");
            }
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd parameter");
    return ESP_FAIL;
}

static esp_err_t auto_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *arr = cJSON_Parse(buf);
    if (arr && cJSON_IsArray(arr)) {
        if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
            num_autos = 0;
            int size = cJSON_GetArraySize(arr);
            for(int i = 0; i < size && i < MAX_AUTOS; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *c = cJSON_GetObjectItem(item, "c");
                cJSON *t = cJSON_GetObjectItem(item, "t");
                cJSON *cmd = cJSON_GetObjectItem(item, "cmd");
                if(c && t && cmd) {
                    auto_rules[num_autos].condition = c->valueint;
                    auto_rules[num_autos].threshold = t->valuedouble;
                    auto_rules[num_autos].command = cmd->valueint;
                    auto_triggered[num_autos] = false; // Freshly created/saved rules can trigger immediately
                    num_autos++;
                }
            }
            xSemaphoreGive(auto_mutex);
        }
        
        cJSON_Delete(arr);
        
        // Push safely to NVS
        nvs_handle_t my_handle;
        if(nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_blob(my_handle, "auto_rules", auto_rules, sizeof(AutoEntry) * num_autos);
            nvs_set_u32(my_handle, "num_autos", num_autos);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        
        httpd_resp_sendstr(req, "OK");
    } else {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON parameters provided");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    time_t now = 0;
    time(&now);
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(timeinfo));
    localtime_r(&now, &timeinfo);
    
    char time_str[64];
    if (timeinfo.tm_year < (2020 - 1900)) {
        strcpy(time_str, "Time not set (Waiting for NTP...)");
    } else {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "last_cmd", last_command_str);
    cJSON_AddNumberToObject(root, "temp", current_temp);
    cJSON_AddNumberToObject(root, "hum", current_hum);
    
    // Package automation rules array
    cJSON *autos_arr = cJSON_CreateArray();
    if (xSemaphoreTake(auto_mutex, portMAX_DELAY)) {
        for(int i = 0; i < num_autos; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddNumberToObject(a, "c", auto_rules[i].condition);
            cJSON_AddNumberToObject(a, "t", auto_rules[i].threshold);
            cJSON_AddNumberToObject(a, "cmd", auto_rules[i].command);
            cJSON_AddItemToArray(autos_arr, a);
        }
        xSemaphoreGive(auto_mutex);
    }
    cJSON_AddItemToObject(root, "autos", autos_arr);
    
    // Package timers array
    cJSON *timers_arr = cJSON_CreateArray();
    if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
        for(int i = 0; i < num_timers; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "d", timers[i].day);
            cJSON_AddNumberToObject(t, "h", timers[i].hour);
            cJSON_AddNumberToObject(t, "m", timers[i].minute);
            cJSON_AddNumberToObject(t, "c", timers[i].command);
            cJSON_AddItemToArray(timers_arr, t);
        }
        xSemaphoreGive(timer_mutex);
    }
    cJSON_AddItemToObject(root, "timers", timers_arr);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t timers_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    cJSON *arr = cJSON_Parse(buf);
    if (arr && cJSON_IsArray(arr)) {
        if (xSemaphoreTake(timer_mutex, portMAX_DELAY)) {
            num_timers = 0;
            int size = cJSON_GetArraySize(arr);
            for(int i = 0; i < size && i < MAX_TIMERS; i++) {
                cJSON *item = cJSON_GetArrayItem(arr, i);
                cJSON *d = cJSON_GetObjectItem(item, "d");
                cJSON *h = cJSON_GetObjectItem(item, "h");
                cJSON *m = cJSON_GetObjectItem(item, "m");
                cJSON *c = cJSON_GetObjectItem(item, "c");
                if(d && h && m && c) {
                    timers[num_timers].day = d->valueint;
                    timers[num_timers].hour = h->valueint;
                    timers[num_timers].minute = m->valueint;
                    timers[num_timers].command = c->valueint;
                    num_timers++;
                }
            }
            xSemaphoreGive(timer_mutex);
        }
        
        cJSON_Delete(arr);
        
        // Push safely to NVS
        nvs_handle_t my_handle;
        if(nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_blob(my_handle, "timers", timers, sizeof(TimerEntry) * num_timers);
            nvs_set_u32(my_handle, "num_timers", num_timers);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
        
        httpd_resp_sendstr(req, "OK");
    } else {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON parameters provided");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = {};
    esp_wifi_scan_start(&scan_config, true);
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    cJSON *root = cJSON_CreateArray();
    for(int i = 0; i < ap_count; i++) cJSON_AddItemToArray(root, cJSON_CreateString((char*)ap_info[i].ssid));
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(ap_info); cJSON_Delete(root); free(json_str);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[200];
    int ret, remaining = req->content_len;
    size_t recv_len = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
    if ((ret = httpd_req_recv(req, buf, recv_len)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if(json) {
        cJSON *s = cJSON_GetObjectItem(json, "ssid");
        cJSON *p = cJSON_GetObjectItem(json, "pass");
        if(s && p) {
            nvs_handle_t my_handle;
            nvs_open("storage", NVS_READWRITE, &my_handle);
            nvs_set_str(my_handle, "wifi_ssid", s->valuestring);
            nvs_set_str(my_handle, "wifi_pass", p->valuestring);
            nvs_set_u8(my_handle, "force_ap", 0); // Auto revert to STA if new settings saved
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "Saved SSID: %s", s->valuestring);
        }
        cJSON_Delete(json);
        httpd_resp_sendstr(req, "OK");
        xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t switch_ap_post_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_set_u8(my_handle, "force_ap", 1);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t switch_wifi_post_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    nvs_open("storage", NVS_READWRITE, &my_handle);
    nvs_set_u8(my_handle, "force_ap", 0);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void start_web_server() {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        
        // Fix the registry slot capacity limits
        config.max_uri_handlers = 20;
        config.uri_match_fn = httpd_uri_match_wildcard;
        
        if (httpd_start(&server, &config) == ESP_OK) {
            httpd_uri_t uri_index  = { .uri = "/",       .method = HTTP_GET,  .handler = index_get_handler,  .user_ctx = NULL };
            httpd_uri_t uri_scan   = { .uri = "/scan",   .method = HTTP_GET,  .handler = scan_get_handler,   .user_ctx = NULL };
            httpd_uri_t uri_save   = { .uri = "/save",   .method = HTTP_POST, .handler = save_post_handler,  .user_ctx = NULL };
            httpd_uri_t uri_ir     = { .uri = "/ir",     .method = HTTP_GET,  .handler = ir_get_handler,     .user_ctx = NULL };
            httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET,  .handler = status_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_timers = { .uri = "/timers", .method = HTTP_POST, .handler = timers_post_handler,.user_ctx = NULL };
            httpd_uri_t uri_auto   = { .uri = "/auto",   .method = HTTP_POST, .handler = auto_post_handler,  .user_ctx = NULL };
            httpd_uri_t uri_switch_ap   = { .uri = "/switch_to_ap",   .method = HTTP_POST, .handler = switch_ap_post_handler,   .user_ctx = NULL };
            httpd_uri_t uri_switch_wifi = { .uri = "/switch_to_wifi", .method = HTTP_POST, .handler = switch_wifi_post_handler, .user_ctx = NULL };
            
            // Common captive portal endpoints
            httpd_uri_t uri_cp1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
            httpd_uri_t uri_cp2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
            httpd_uri_t uri_cp3 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
            
            // Catch-all fallback endpoint to seamlessly suppress '/connecttest.txt' and redirection requests
            httpd_uri_t uri_fallback = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_redirect, .user_ctx = NULL };
            
            httpd_register_uri_handler(server, &uri_index);
            httpd_register_uri_handler(server, &uri_scan);
            httpd_register_uri_handler(server, &uri_save);
            httpd_register_uri_handler(server, &uri_ir);
            httpd_register_uri_handler(server, &uri_status);
            httpd_register_uri_handler(server, &uri_timers);
            httpd_register_uri_handler(server, &uri_auto);
            httpd_register_uri_handler(server, &uri_switch_ap);
            httpd_register_uri_handler(server, &uri_switch_wifi);
            httpd_register_uri_handler(server, &uri_cp1);
            httpd_register_uri_handler(server, &uri_cp2);
            httpd_register_uri_handler(server, &uri_cp3);
            
            // Register wildcard path last
            httpd_register_uri_handler(server, &uri_fallback);
            
            ESP_LOGI(TAG, "Web Server started successfully on port %d", config.server_port);
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

    // Safely stop Wi-Fi driver first to apply mode modifications cleanly 
    esp_wifi_stop();

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

    ESP_LOGI(TAG, "SoftAP broadcast started! SSID: ESP32_AC_Config, WPA2 Password: 12345678, IP: 192.168.4.1");
    
    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 5, &dns_task_handle);
    }
    start_web_server();
}

void boot_button_task(void *pvParameter) {
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);
    while (1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
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

/* ==============================================
   REPL CONSOLE TASK
   ============================================== */
static void console_read_task(void *pvParameter) {
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
                if (buf[i] == 0x03) { // Ctrl+C (Interrupt)
                    ESP_LOGW(TAG, "[Sent Ctrl+C - Interrupt (Sending AC OFF)]");
                    execute_command(0); // Send AC OFF command as an emergency stop equivalent
                } else if (buf[i] == 0x04) { // Ctrl+D (Soft Reboot)
                    ESP_LOGW(TAG, "[Sent Ctrl+D - Soft Reboot]");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else if (buf[i] == '\r' || buf[i] == '\n') {
                    if (cmd_idx > 0) {
                        cmd[cmd_idx] = '\0';
                        if (strcmp(cmd, "reset") == 0) {
                            ESP_LOGW(TAG, "Command 'reset' received. Factory Resetting NVS...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_all(h);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "ap") == 0) {
                            ESP_LOGW(TAG, "Command 'ap' received. Switching to AP Mode...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "force_ap", 1);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcmp(cmd, "wifi") == 0) {
                            ESP_LOGW(TAG, "Command 'wifi' received. Switching to Wi-Fi Mode...");
                            nvs_handle_t h;
                            if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "force_ap", 0);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(500));
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

/* ==============================================
   WIFI EVENT HANDLER & STATE MACHINE
   ============================================== */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA Interface started. Connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_fallback_active) {
            ESP_LOGI(TAG, "Disconnected from STA. Configuration portal is currently active.");
            return;
        }
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to local router (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            ESP_LOGW(TAG, "Connection attempts exhausted. Rolling back to AP Config Portal...");
            s_ap_fallback_active = true;
            start_ap_server();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP mode interface is up.");
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
        
        // Start mDNS and Control Server once connected and assigned an IP
        ESP_ERROR_CHECK(mdns_init());
        mdns_hostname_set("esp32-ac-ctrl");
        mdns_instance_name_set("ESP32 AC Controller");
        mdns_service_add("ESP32 AC Controller", "http", "tcp", 80, NULL, 0); 
        
        if (!server_disabled_by_timer) {
            start_web_server();
        }
    }
}

/* ==============================================
   MAIN
   ============================================== */
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register Event Handlers to track connect/disconnect statuses
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    init_uart();
    
    timer_mutex = xSemaphoreCreateMutex();
    auto_mutex = xSemaphoreCreateMutex();
    
    // Resume established logic rules securely stored in NV memory
    load_timers();
    load_auto_config();
    
    // Start timers and temp polling tasks
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);
    xTaskCreate(dht_task, "dht_task", 4096, NULL, 5, NULL);

    nvs_handle_t my_handle;
    bool has_credentials = false;
    uint8_t force_ap_u8 = 0;

    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u8(my_handle, "force_ap", &force_ap_u8);
        char ssid[32]; char pass[64];
        size_t s_len = sizeof(ssid); size_t p_len = sizeof(pass);
        
        if (force_ap_u8 == 0 &&
            nvs_get_str(my_handle, "wifi_ssid", ssid, &s_len) == ESP_OK &&
            nvs_get_str(my_handle, "wifi_pass", pass, &p_len) == ESP_OK) {
            
            ESP_LOGI(TAG, "Saved network credentials found. Initializing connection sequence to: %s", ssid);
            wifi_config_t wifi_config = {};
            strcpy((char*)wifi_config.sta.ssid, ssid);
            strcpy((char*)wifi_config.sta.password, pass);
            
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());
            has_credentials = true;
        }
        nvs_close(my_handle);
    }

    // Fallback directly to AP mode if no settings are configured or if manually forced
    if (!has_credentials || force_ap_u8 == 1) {
        if(force_ap_u8 == 1) ESP_LOGW(TAG, "AP Mode forced by User Preference.");
        else ESP_LOGW(TAG, "No Wi-Fi credentials found in memory. Launching AP Config Portal...");
        
        s_ap_fallback_active = true;
        start_ap_server();
    }

    // Start Boot Button Listener (Allows forcing configuration mode via physical button)
    xTaskCreate(boot_button_task, "button_task", 2048, NULL, 10, NULL);
    
    // Start standard input keyboard listener for REPL commands
    xTaskCreate(console_read_task, "console_read_task", 4096, NULL, 5, NULL);
}