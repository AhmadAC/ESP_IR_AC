#include "ir_uart.h"
#include "ir_codes.h"
#include "config.h"
#include "web_server.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "IR_UART";
char last_command_str[32] = "None";

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
    
    frame[0] = 0x68;                     
    frame[1] = frame_len & 0xFF;         
    frame[2] = (frame_len >> 8) & 0xFF;  
    frame[3] = 0x00;                     
    frame[4] = 0x22;                     

    uint16_t checksum = 0x00 + 0x22;
    for(size_t i = 0; i < data_len; i++) {
        frame[5 + i] = data[i];
        checksum += data[i];
    }

    frame[5 + data_len] = checksum % 256;
    frame[6 + data_len] = 0x16;           

    uart_write_bytes(UART_PORT_NUM, (const char*)frame, frame_len);
    ESP_LOGI(TAG, "Sent IR Command via UART (Length: %zu)", frame_len);
    free(frame);
}

const char* get_command_string(int cmd) {
    switch (cmd) {
        case 0: return "AC OFF";
        case 1: return "AC ON";
        case 2: return "Set 19C";
        case 3: return "Set 20C";
        case 4: return "Set 21C";
        case 5: return "Set 22C";
        case 6: return "Set 23C";
        case 7: return "Set 24C";
        case 8: return "Set 25C";
        case 9: return "Set 26C";
        case 10: return "Set 27C";
        case 11: return "Mode HOT";
        case 12: return "Mode COLD";
        case 13: return "Server OFF";
        case 14: return "Server ON";
        default: return "Unknown";
    }
}

void execute_command(int cmd) {
    switch (cmd) {
        case 0:  strcpy(last_command_str, "AC OFF"); send_uart_ir(ir_off, ir_off_len); break;
        case 1:  strcpy(last_command_str, "AC ON"); send_uart_ir(ir_on, ir_on_len); break;
        case 2:  strcpy(last_command_str, "Set 19C"); send_uart_ir(ir_19c, ir_19c_len); break;
        case 3:  strcpy(last_command_str, "Set 20C"); send_uart_ir(ir_20c, ir_20c_len); break;
        case 4:  strcpy(last_command_str, "Set 21C"); send_uart_ir(ir_21c, ir_21c_len); break;
        case 5:  strcpy(last_command_str, "Set 22C"); send_uart_ir(ir_22c, ir_22c_len); break;
        case 6:  strcpy(last_command_str, "Set 23C"); send_uart_ir(ir_23c, ir_23c_len); break;
        case 7:  strcpy(last_command_str, "Set 24C"); send_uart_ir(ir_24c, ir_24c_len); break;
        case 8:  strcpy(last_command_str, "Set 25C"); send_uart_ir(ir_25c, ir_25c_len); break;
        case 9:  strcpy(last_command_str, "Set 26C"); send_uart_ir(ir_26c, ir_26c_len); break;
        case 10: strcpy(last_command_str, "Set 27C"); send_uart_ir(ir_27c, ir_27c_len); break;
        case 11: strcpy(last_command_str, "Mode HOT"); send_uart_ir(ir_hot, ir_hot_len); break;
        case 12: strcpy(last_command_str, "Mode COLD"); send_uart_ir(ir_cold, ir_cold_len); break;
        case 13: strcpy(last_command_str, "Server OFF"); stop_web_server(); break;
        case 14: strcpy(last_command_str, "Server ON"); start_web_server(); break;
        default: ESP_LOGW(TAG, "Unknown command ID: %d", cmd); break;
    }
}