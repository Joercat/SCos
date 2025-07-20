
#include "../include/stdint.h"

void vga_put_char(int x, int y, char c, uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;
    int index = 2 * (y * 80 + x);
    video[index] = c;
    video[index + 1] = color;
}

void vga_put_string(int x, int y, const char* str, uint8_t color) {
    int i = 0;
    while (str[i] && x + i < 80) {
        vga_put_char(x + i, y, str[i], color);
        i++;
    }
}

void vga_clear_screen(uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video[i] = ' ';
        video[i + 1] = color;
    }
}

void vga_clear_line(int y, uint8_t color) {
    volatile char* video = (volatile char*)0xB8000;
    for (int x = 0; x < 80; x++) {
        int index = 2 * (y * 80 + x);
        video[index] = ' ';
        video[index + 1] = color;
    }
}

void vga_draw_box(int x, int y, int width, int height, uint8_t color) {
    // Top and bottom borders
    for (int i = 0; i < width; i++) {
        vga_put_char(x + i, y, '-', color);
        vga_put_char(x + i, y + height - 1, '-', color);
    }
    // Left and right borders
    for (int i = 0; i < height; i++) {
        vga_put_char(x, y + i, '|', color);
        vga_put_char(x + width - 1, y + i, '|', color);
    }
    // Corners
    vga_put_char(x, y, '+', color);
    vga_put_char(x + width - 1, y, '+', color);
    vga_put_char(x, y + height - 1, '+', color);
    vga_put_char(x + width - 1, y + height - 1, '+', color);
}

void center_text(int y, const char* text, uint8_t color) {
    int len = 0;
    while (text[len]) len++; // strlen
    int x = (80 - len) / 2;
    vga_put_string(x, y, text, color);
}

void draw_horizontal_line(int y, int x1, int x2, char c, uint8_t color) {
    for (int x = x1; x <= x2; ++x) {
        vga_put_char(x, y, c, color);
    }
}
