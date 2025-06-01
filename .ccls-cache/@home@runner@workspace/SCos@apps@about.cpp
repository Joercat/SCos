// Removed VGA function definitions and included vga_utils.hpp
#include <stdint.h>
#include "../include/string.h"
#include "about.hpp"
#include "../ui/vga_utils.hpp"

// VGA text mode constants
#define VGA_BUFFER ((volatile char*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BYTES_PER_CHAR 2

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

// Utility functions
static int about_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

void openAbout() {
    // Clear the screen first
    uint8_t bg_color = MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_BLUE);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        vga_clear_line(y, bg_color);
    }

    // Draw a decorative border
    vga_draw_box(5, 3, 70, 18, MAKE_COLOR(COLOR_WHITE, COLOR_BLUE));

    // Title with highlighted background
    uint8_t title_color = MAKE_COLOR(COLOR_YELLOW, COLOR_BLUE);
    center_text(5, "SCos Operating System", title_color);
    center_text(6, "=====================", title_color);

    // Version information
    uint8_t info_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    center_text(8, "Version: 1.3.0", info_color);
    center_text(9, "Build Date: 2025-05-30", info_color);
    center_text(10, "Architecture: x86", info_color);

    // System information
    center_text(12, "System Information:", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLUE));
    center_text(13, "Memory Model: unknown", info_color);
    center_text(14, "Boot Mode: Protected Mode", info_color);
    center_text(15, "Display: VGA Text Mode 80x25", info_color);

    // Copyright notice at bottom
    uint8_t copyright_color = MAKE_COLOR(COLOR_DARK_GRAY, COLOR_BLUE);
    center_text(23, "(c) 2025 SCos Project", copyright_color);
}

void About::handleInput(uint8_t key) {
    // Handle about input - placeholder for future implementation
    // Can add navigation or close functionality here
}
