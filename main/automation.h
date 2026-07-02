#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"

typedef struct {
    int day;
    int hour;
    int minute;
    int command;
} TimerEntry;

typedef struct {
    float threshold;
    int condition; // 0 = Below or equal, 1 = Above or equal
    int command;
} AutoEntry;

extern TimerEntry timers[MAX_TIMERS];
extern int num_timers;
extern SemaphoreHandle_t timer_mutex;

extern AutoEntry auto_rules[MAX_AUTOS];
extern bool auto_triggered[MAX_AUTOS];
extern int num_autos;
extern SemaphoreHandle_t auto_mutex;

void init_automation();
void load_timers();
void load_auto_config();
void timer_task(void *pvParameter);