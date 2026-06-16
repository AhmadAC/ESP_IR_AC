#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "mdns.h"

#define UART_PORT_NUM UART_NUM_1
#define TXD_PIN (GPIO_NUM_17) // Wire to IR Module RXD
#define RXD_PIN (GPIO_NUM_18) // Wire to IR Module TXD
#define BOOT_BUTTON_PIN GPIO_NUM_0 
#define MAXIMUM_RETRY 5

static const char *TAG = "AC_CTRL";
httpd_handle_t server = NULL;
static int s_retry_num = 0;
static bool s_ap_fallback_active = false;
char last_command_str[32] = "None";

/* ==============================================
   RAW IR MIDEA CODES
   ============================================== */
const uint8_t ir_off[] = {
    0xA5, 0x04, 0xA7, 0x04, 0x42, 0xCE, 0x01, 0x3E, 0x4A, 0x3F, 0xD1, 0x01, 0x3C, 0xD4, 0x01, 0x3C, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3F, 0xD1, 0x01, 0x38, 0x4E, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3F, 0x47, 0x3B, 0xD4, 0x01, 0x3F, 0x46, 0x3A, 0xD6, 0x01, 0x38, 0xD7, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0x4A, 0x3A, 0xD6, 0x01, 0x3F, 0x47, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0x4C, 0x39, 0x4C, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x93, 0x05, 0xA1, 0x04, 0xAD, 0x04, 0x39, 0xD6, 0x01, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x39, 0xD6, 0x01, 0x39, 0x4C, 0x3A, 0x4C, 0x39, 0xD6, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4B, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x3F, 0xD0, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3C, 0x4A, 0x39, 0x4C, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x39, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0xD4, 0x01, 0x39, 0x4C, 0x3B, 0x4A, 0x3C, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD5, 0x01, 0x3C
};

const uint8_t ir_on[] = {
    0xA0, 0x04, 0xAB, 0x04, 0x3F, 0xD1, 0x01, 0x3F, 0x4A, 0x3B, 0xD4, 0x01, 0x3C, 0xD4, 0x01, 0x3B, 0x4B, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3C, 0x4A, 0x3A, 0xD6, 0x01, 0x3B, 0xD5, 0x01, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x38, 0xD8, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0x4A, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x39, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0x93, 0x05, 0xA0, 0x04, 0xAF, 0x04, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x39, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x3A, 0x4C, 0x3C, 0x4A, 0x3C, 0xD4, 0x01, 0x38, 0x4D, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0x4A, 0x3A, 0x4C, 0x3C, 0x4A, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3C, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x38, 0x4E, 0x38, 0x4E, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3C
};

const uint8_t ir_19c[] = {
    0xA2, 0x04, 0xAA, 0x04, 0x3F, 0xD1, 0x01, 0x3E, 0x4A, 0x3C, 0xD7, 0x01, 0x39, 0xD4, 0x01, 0x3C, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3C, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x39, 0x4C, 0x3A, 0x4B, 0x3A, 0x4C, 0x38, 0x4D, 0x3A, 0x4C, 0x39, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x39, 0x4C, 0x39, 0x4C, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x38, 0xD8, 0x01, 0x3A, 0x95, 0x05, 0xA0, 0x04, 0xAF, 0x04, 0x3C, 0xD4, 0x01, 0x3E, 0x48, 0x3B, 0xD4, 0x01, 0x38, 0xD8, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x38, 0xD7, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD5, 0x01, 0x3A, 0xD6, 0x01, 0x3C, 0x4A, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0x4A, 0x3B, 0x4A, 0x38, 0x4D, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x38, 0xD7, 0x01, 0x3A, 0x4B, 0x3A, 0x4C, 0x3C, 0x4A, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A
};

const uint8_t ir_24c[] = {
    0xA0, 0x04, 0xAC, 0x04, 0x3F, 0xD1, 0x01, 0x3F, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0x4A, 0x3C, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x38, 0xD8, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3C, 0x4A, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3A, 0x4C, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0x4A, 0x3B, 0x4A, 0x3A, 0x4C, 0x3A, 0xD5, 0x01, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0xD4, 0x01, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0x93, 0x05, 0xA0, 0x04, 0xAF, 0x04, 0x3A, 0xD6, 0x01, 0x3B, 0x4A, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x39, 0x4C, 0x3C, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x38, 0x4E, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x38, 0x4E, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3A, 0xD6, 0x01, 0x38, 0xD7, 0x01, 0x3C, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3C, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x3C, 0x4A, 0x3A, 0x4C, 0x3C, 0x4A, 0x3B, 0x4A, 0x3B, 0xD4, 0x01, 0x3B, 0x4A, 0x3A, 0x4C, 0x3C, 0x4A, 0x3A, 0x4C, 0x3B, 0x4A, 0x3A, 0x4C, 0x3B, 0xD4, 0x01, 0x3A, 0x4C, 0x3A, 0xD6, 0x01, 0x39, 0xD6, 0x01, 0x3C, 0xD4, 0x01, 0x3A, 0xD6, 0x01, 0x3B, 0xD4, 0x01, 0x3B, 0xD4, 0x01, 0x3B
};

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
   HTTP SERVER (CONTROL & PROVISIONING)
   ============================================== */
void delayed_reboot_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    char *html = (char *)malloc(2500);
    snprintf(html, 2500,
        "<!DOCTYPE html><html><head><title>ESP32 A/C Controller</title><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>button{padding:10px;margin:5px;font-size:16px;}</style></head>"
        "<body style='font-family:sans-serif; margin:20px;'>"
        "<h2>Control A/C</h2>"
        "<p><b>Last Command:</b> <span id='lastcmd' style='color:#007BFF;'>%s</span></p>"
        "<button onclick='c(0)'>Turn OFF</button> <button onclick='c(1)'>Turn ON</button><br>"
        "<button onclick='c(2)'>Set 19&deg;C</button> <button onclick='c(3)'>Set 24&deg;C</button>"
        "<p id='cstat'></p><hr>"
        "<h2>Wi-Fi Setup</h2>"
        "<button onclick='scan()'>Scan Networks</button><p id='status'></p>"
        "<div style='margin-bottom:10px;'><label>SSID:</label><br><select id='ssid'></select></div>"
        "<div style='margin-bottom:10px;'><label>Password:</label><br><input type='password' id='pass'> "
        "<input type='checkbox' onclick=\"let p=document.getElementById('pass'); p.type=(p.type==='password')?'text':'password';\"> Show</div>"
        "<button onclick='save()'>Save & Reboot</button>"
        "<script>"
        "function c(cmd) {"
        "  document.getElementById('cstat').innerText = 'Sending...';"
        "  fetch('/ir?cmd='+cmd).then(r=>r.text()).then(t=>{"
        "    document.getElementById('cstat').innerText = 'Status: ' + t;"
        "    setTimeout(() => location.reload(), 600);"
        "  }).catch(e=>{"
        "    document.getElementById('cstat').innerText = 'Error: ' + e;"
        "  });"
        "}"
        "function scan() {"
        "  document.getElementById('status').innerText = 'Scanning...';"
        "  fetch('/scan').then(r=>r.json()).then(d=>{"
        "    let s = document.getElementById('ssid'); s.innerHTML = '';"
        "    d.forEach(n=>{ s.innerHTML += '<option value=\"'+n+'\">'+n+'</option>'; });"
        "    document.getElementById('status').innerText = 'Found ' + d.length + ' networks';"
        "  });"
        "}"
        "function save() {"
        "  let s = document.getElementById('ssid').value;"
        "  let p = document.getElementById('pass').value;"
        "  fetch('/save', {method:'POST', body:JSON.stringify({ssid:s, pass:p})})"
        "  .then(()=>{ alert('Credentials saved! ESP32 rebooting in 2 seconds.'); });"
        "}"
        "</script></body></html>", last_command_str);
    
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ESP_OK;
}

static esp_err_t ir_get_handler(httpd_req_t *req) {
    char buf[50];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[10];
        if (httpd_query_key_value(buf, "cmd", param, sizeof(param)) == ESP_OK) {
            int cmd = atoi(param);
            ESP_LOGI(TAG, "Received IR Web Command: %d", cmd);
            if (cmd == 0) {
                strcpy(last_command_str, "AC OFF");
                send_uart_ir(ir_off, sizeof(ir_off));
            }
            else if (cmd == 1) {
                strcpy(last_command_str, "AC ON");
                send_uart_ir(ir_on, sizeof(ir_on));
            }
            else if (cmd == 2) {
                strcpy(last_command_str, "Set 19C");
                send_uart_ir(ir_19c, sizeof(ir_19c));
            }
            else if (cmd == 3) {
                strcpy(last_command_str, "Set 24C");
                send_uart_ir(ir_24c, sizeof(ir_24c));
            }
            httpd_resp_sendstr(req, "Command Sent");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd parameter");
    return ESP_FAIL;
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
        const char *ssid = cJSON_GetObjectItem(json, "ssid")->valuestring;
        const char *pass = cJSON_GetObjectItem(json, "pass")->valuestring;
        nvs_handle_t my_handle;
        nvs_open("storage", NVS_READWRITE, &my_handle);
        nvs_set_str(my_handle, "wifi_ssid", ssid);
        nvs_set_str(my_handle, "wifi_pass", pass);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Saved SSID: %s", ssid);
        cJSON_Delete(json);
        httpd_resp_sendstr(req, "OK");
        xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

void start_web_server() {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        if (httpd_start(&server, &config) == ESP_OK) {
            httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
            httpd_uri_t uri_ir   = { .uri = "/ir", .method = HTTP_GET, .handler = ir_get_handler, .user_ctx = NULL };
            httpd_register_uri_handler(server, &uri_index);
            httpd_register_uri_handler(server, &uri_scan);
            httpd_register_uri_handler(server, &uri_save);
            httpd_register_uri_handler(server, &uri_ir);
            ESP_LOGI(TAG, "Web Server started successfully on port %d", config.server_port);
        }
    }
}

void start_ap_server() {
    ESP_LOGI(TAG, "Starting AP Mode...");
    
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "ESP32_Config");
    strcpy((char*)wifi_config.ap.password, "12345678");
    wifi_config.ap.ssid_len = strlen("ESP32_Config");
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

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
   WIFI EVENT HANDLER & STATE MACHINE
   ============================================== */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_fallback_active) {
            ESP_LOGI(TAG, "Disconnected from STA, but AP config portal is running. Retries paused.");
            return;
        }
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to local Wi-Fi router (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            ESP_LOGW(TAG, "Connection failed. Launching AP Config Portal automatically.");
            s_ap_fallback_active = true;
            start_ap_server();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        
        // Start mDNS and Control Server once connected and assigned an IP
        ESP_ERROR_CHECK(mdns_init());
        mdns_hostname_set("esp32-ac-ctrl");
        mdns_instance_name_set("ESP32 AC Controller");
        mdns_service_add("ESP32 AC Controller", "http", "tcp", 80, NULL, 0); // Advertise the HTTP service for discovery!
        
        start_web_server();
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

    nvs_handle_t my_handle;
    bool has_credentials = false;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        char ssid[32]; char pass[64];
        size_t s_len = sizeof(ssid); size_t p_len = sizeof(pass);
        if (nvs_get_str(my_handle, "wifi_ssid", ssid, &s_len) == ESP_OK &&
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

    // Fallback directly to AP mode if no settings are configured
    if (!has_credentials) {
        ESP_LOGW(TAG, "No Wi-Fi credentials found in memory. Launching AP Config Portal...");
        s_ap_fallback_active = true;
        start_ap_server();
    }

    // Start Boot Button Listener (Allows forcing configuration mode)
    xTaskCreate(boot_button_task, "button_task", 2048, NULL, 10, NULL);
}