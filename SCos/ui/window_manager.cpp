#include "window_manager.hpp"
#include <stdint.h>

// Include the vga_utils header instead of defining the functions here.
#include "vga_utils.hpp"

// Local string function implementations for freestanding environment
static int strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

// VGA text mode constants
const int VGA_WIDTH = 80;
const int VGA_HEIGHT = 25;
static volatile char* video_memory = (volatile char*)0xB8000;

// Window management
static Window windows[MAX_WINDOWS];
static int window_count = 0;
static int active_window = -1;

void WindowManager::init() {
    window_count = 0;
    active_window = -1;
    clearScreen();
}

void WindowManager::clearScreen() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT * 2; i += 2) {
        video_memory[i] = ' ';
        video_memory[i + 1] = 0x07; // Light grey on black
    }
}

int WindowManager::createWindow(const char* title, int x, int y, int width, int height) {
    if (window_count >= MAX_WINDOWS) {
        return -1; // No more windows available
    }

    int id = window_count++;
    windows[id].id = id;
    windows[id].x = x;
    windows[id].y = y;
    windows[id].width = width;
    windows[id].height = height;
    windows[id].visible = true;
    windows[id].focused = false;

    // Copy title
    int i = 0;
    while (title[i] && i < MAX_TITLE_LENGTH - 1) {
        windows[id].title[i] = title[i];
        i++;
    }
    windows[id].title[i] = '\0';

    drawWindow(id);
    return id;
}

void WindowManager::drawWindow(int window_id) {
    if (window_id < 0 || window_id >= window_count) return;

    Window& win = windows[window_id];
    if (!win.visible) return;

    uint8_t border_color = win.focused ? 0x4F : 0x70; // Red if focused, grey otherwise
    uint8_t bg_color = 0x17; // Grey background
    uint8_t title_color = 0x4F; // Red on white for title

    // Draw window border and background
    for (int j = 0; j < win.height; ++j) {
        for (int i = 0; i < win.width; ++i) {
            int screen_x = win.x + i;
            int screen_y = win.y + j;

            if (screen_x >= VGA_WIDTH || screen_y >= VGA_HEIGHT) continue;

            int idx = 2 * (screen_y * VGA_WIDTH + screen_x);

            if (i == 0 || i == win.width - 1 || j == 0 || j == win.height - 1) {
                video_memory[idx] = (i == 0 || i == win.width - 1) ? '|' : '-'; // Border
                video_memory[idx + 1] = border_color;
            } else {
                video_memory[idx] = ' '; // Interior
                video_memory[idx + 1] = bg_color;
            }
        }
    }

    // Draw title
    int title_len = strlen(win.title);
    int title_start = win.x + 2;
    for (int i = 0; i < title_len && i < win.width - 4; ++i) {
        int idx = 2 * (win.y * VGA_WIDTH + title_start + i);
        video_memory[idx] = win.title[i];
        video_memory[idx + 1] = title_color;
    }
}

void WindowManager::closeWindow(int window_id) {
    if (window_id < 0 || window_id >= window_count) return;

    windows[window_id].visible = false;
    clearWindowArea(windows[window_id].x, windows[window_id].y, 
                   windows[window_id].width, windows[window_id].height);

    if (active_window == window_id) {
        active_window = -1;
    }
}

void WindowManager::moveWindow(int window_id, int x, int y) {
    if (window_id < 0 || window_id >= window_count) return;

    // Clear old position
    clearWindowArea(windows[window_id].x, windows[window_id].y,
                   windows[window_id].width, windows[window_id].height);

    // Update position
    windows[window_id].x = x;
    windows[window_id].y = y;

    // Redraw
    drawWindow(window_id);
}

void WindowManager::resizeWindow(int window_id, int width, int height) {
    if (window_id < 0 || window_id >= window_count) return;

    // Clear old window
    clearWindowArea(windows[window_id].x, windows[window_id].y,
                   windows[window_id].width, windows[window_id].height);

    // Update size
    windows[window_id].width = width;
    windows[window_id].height = height;

    // Redraw
    drawWindow(window_id);
}

void WindowManager::setActiveWindow(int window_id) {
    if (window_id < 0 || window_id >= window_count) return;

    // Unfocus previous window
    if (active_window >= 0 && active_window < window_count) {
        windows[active_window].focused = false;
        drawWindow(active_window);
    }

    // Focus new window
    active_window = window_id;
    windows[window_id].focused = true;
    drawWindow(window_id);
}

void WindowManager::clearWindowArea(int x, int y, int width, int height) {
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            int screen_x = x + i;
            int screen_y = y + j;

            if (screen_x >= VGA_WIDTH || screen_y >= VGA_HEIGHT) continue;

            int idx = 2 * (screen_y * VGA_WIDTH + screen_x);
            video_memory[idx] = ' ';
            video_memory[idx + 1] = 0x07; // Default color
        }
    }
}

Window* WindowManager::getWindow(int window_id) {
    if (window_id < 0 || window_id >= window_count) return static_cast<Window*>(nullptr);
    return &windows[window_id];
}

int WindowManager::getActiveWindow() {
    return active_window;
}

void WindowManager::refreshAll() {
    clearScreen();
    for (int i = 0; i < window_count; ++i) {
        if (windows[i].visible) {
            drawWindow(i);
        }
    }
}