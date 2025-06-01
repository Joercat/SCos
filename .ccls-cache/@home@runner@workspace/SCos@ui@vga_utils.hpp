
#ifndef VGA_UTILS_HPP
#define VGA_UTILS_HPP

#include "../include/stdint.h"

void vga_put_char(int x, int y, char c, uint8_t color);
void vga_put_string(int x, int y, const char* str, uint8_t color);
void vga_clear_screen(uint8_t color);
void vga_clear_line(int y, uint8_t color);
void vga_draw_box(int x, int y, int width, int height, uint8_t color);
void center_text(int y, const char* text, uint8_t color);
void draw_horizontal_line(int y, int x1, int x2, char c, uint8_t color);

#endif
