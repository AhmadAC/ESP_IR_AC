// main/ir_uart.h
#pragma once

extern char last_command_str[32];
extern int last_command_id;

void init_uart();
void execute_command(int cmd);