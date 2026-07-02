#pragma once

extern char last_command_str[32];

void init_uart();
void execute_command(int cmd);
const char* get_command_string(int cmd);