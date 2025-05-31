
#include "file_manager.hpp"
#include <stdint.h>

// VGA text mode constants
#define VGA_BUFFER ((volatile char*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BYTES_PER_CHAR 2

// Color attributes
#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x01
#define COLOR_WHITE 0x0F
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_YELLOW 0x0E

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

static void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile char* pos = VGA_BUFFER + (y * VGA_WIDTH + x) * VGA_BYTES_PER_CHAR;
        pos[0] = c;
        pos[1] = color;
    }
}

static void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < VGA_WIDTH; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

// Local string function implementations for freestanding environment
static int fm_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

void openFileManager() {
    uint8_t header_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    uint8_t text_color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);
    uint8_t folder_color = MAKE_COLOR(COLOR_YELLOW, COLOR_WHITE);
    
    // Clear area and draw file manager
    for (int y = 2; y < 20; y++) {
        for (int x = 2; x < 37; x++) {
            vga_put_char(x, y, ' ', text_color);
        }
    }
    
    vga_put_string(10, 3, "File Manager", header_color);
    vga_put_string(3, 4, "Path: /home", text_color);
    
    // Draw file list
    vga_put_string(4, 6, "[DIR] home", folder_color);
    vga_put_string(4, 7, "[DIR] apps", folder_color);
    vga_put_string(4, 8, "[DIR] system", folder_color);
    vga_put_string(4, 9, "welcome.txt", text_color);
    vga_put_string(4, 10, "readme.txt", text_color);
    
    // Status line
    vga_put_string(4, 18, "5 items | 2 folders, 3 files", MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_WHITE));
}

void FileManager::handleInput(uint8_t key) {
    // Handle file manager input
    switch (key) {
        case 0x01: // Escape
            // Close file manager
            break;
        case 0x48: // Up arrow
            // Navigate up
            break;
        case 0x50: // Down arrow
            // Navigate down
            break;
        case 0x1C: // Enter
            // Open selected item
            break;
        default:
            // Handle other keys
            break;
    }
}
