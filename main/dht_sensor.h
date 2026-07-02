#pragma once
// main/dht_sensor.h

extern float current_temp;
extern float current_hum;

void dht_task(void *pvParameter);
void reset_automation_triggers();