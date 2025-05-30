#include <stdint.h>

// VGA text mode constants
#define VGA_BUFFER ((volatile char*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BYTES_PER_CHAR 2

// Color attributes
#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x01
#define COLOR_GREEN 0x02
#define COLOR_CYAN 0x03
#define COLOR_RED 0x04
#define COLOR_MAGENTA 0x05
#define COLOR_BROWN 0x06
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_DARK_GRAY 0x08
#define COLOR_LIGHT_BLUE 0x09
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_LIGHT_CYAN 0x0B
#define COLOR_LIGHT_RED 0x0C
#define COLOR_LIGHT_MAGENTA 0x0D
#define COLOR_YELLOW 0x0E
#define COLOR_WHITE 0x0F

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

// Utility functions
static int strlen_custom(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

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

static void vga_clear_line(int y, uint8_t color) {
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put_char(x, y, ' ', color);
    }
}

static void vga_draw_box(int x, int y, int width, int height, uint8_t color) {
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

static void center_text(int y, const char* text, uint8_t color) {
    int len = strlen_custom(text);
    int x = (VGA_WIDTH - len) / 2;
    vga_put_string(x, y, text, color);
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

// Enhanced version with system info detection
void openAboutAdvanced() {
    // Clear screen
    uint8_t bg_color = MAKE_COLOR(COLOR_BLACK, COLOR_BLACK);
    for (int y = 0; y < VGA_HEIGHT; y++) {
        vga_clear_line(y, bg_color);
    }
    
    // Gradient-like effect for title area
    for (int y = 0; y < 5; y++) {
        uint8_t gradient_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE + y % 3);
        vga_clear_line(y, gradient_color);
    }
    
    // Main content area
    for (int y = 5; y < 20; y++) {
        vga_clear_line(y, MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_BLACK));
    }
    
    // Bottom status bar
    for (int y = 20; y < VGA_HEIGHT; y++) {
        vga_clear_line(y, MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY));
    }
    
    // Title section
    center_text(1, "S C o s", MAKE_COLOR(COLOR_WHITE, COLOR_BLUE));
    center_text(2, "SCos Operating System", MAKE_COLOR(COLOR_YELLOW, COLOR_BLUE));
    
    // Main info section
    vga_put_string(10, 6, "Version:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 6, "1.3.0 (Stable)", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    vga_put_string(10, 7, "Kernel:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 7, "Monolithic", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    vga_put_string(10, 8, "Architecture:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 8, "Intel x86 (32-bit)", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    vga_put_string(10, 9, "Memory:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 9, "Protected Mode", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    vga_put_string(10, 10, "Graphics:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 10, "VGA Compatible", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    vga_put_string(10, 11, "File System:", MAKE_COLOR(COLOR_CYAN, COLOR_BLACK));
    vga_put_string(25, 11, "Custom FS", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    // Features section
    center_text(13, "Key Features:", MAKE_COLOR(COLOR_LIGHT_MAGENTA, COLOR_BLACK));
    vga_put_string(15, 14, "* Preemptive Multitasking", MAKE_COLOR(COLOR_GREEN, COLOR_BLACK));
    vga_put_string(15, 15, "* Memory Protection", MAKE_COLOR(COLOR_GREEN, COLOR_BLACK));
    vga_put_string(15, 16, "* Device Driver Framework", MAKE_COLOR(COLOR_GREEN, COLOR_BLACK));
    vga_put_string(15, 17, "* System Call Interface", MAKE_COLOR(COLOR_GREEN, COLOR_BLACK));
    
    // Status bar
    vga_put_string(2, 21, "Status: Running", MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY));
    vga_put_string(25, 21, "Uptime: 00:02:34", MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY));
    vga_put_string(50, 21, "Memory: unknown", MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY));
    
    // Footer
    center_text(23, "ESC: Exit | F1: Help | F5: Refresh", MAKE_COLOR(COLOR_DARK_GRAY, COLOR_BLACK));
}