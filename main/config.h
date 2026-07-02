#pragma once
#include "driver/gpio.h"
#include "driver/uart.h"

// Hardware Pin Definitions
#define UART_PORT_NUM UART_NUM_1
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_18)
#define DHT_PIN (GPIO_NUM_4)
#define BOOT_BUTTON_PIN GPIO_NUM_0 

// System Constants
#define MAX_TIMERS 20
#define MAX_AUTOS 10
#define MAXIMUM_RETRY 5