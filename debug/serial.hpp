#pragma once

bool init_serial();
void serial_write(const char* data);
void serial_printf(const char* format, ...);