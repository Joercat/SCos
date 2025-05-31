
#include "file_manager.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

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
