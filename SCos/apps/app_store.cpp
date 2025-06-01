#include "app_store.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Local string function implementations for freestanding environment
static int app_store_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void app_store_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// App Store state
static int store_window_id = -1;
static bool store_visible = false;
static int selected_app = 0;

struct StoreApp {
    const char* name;
    const char* description;
    const char* version;
    bool installed;
};

static StoreApp available_apps[] = {
    {"Text Editor Pro", "Advanced text editing", "v2.1", false},
    {"Math Calculator", "Scientific calculator", "v1.5", true},
    {"Image Viewer", "View image files", "v1.0", false},
    {"Music Player", "Play audio files", "v3.2", false},
    {"Code Editor", "Programming IDE", "v4.0", false},
    {"Web Browser+", "Enhanced web browser", "v2.8", false}
};

static const int app_count = 6;

void AppStore::init() {
    store_visible = false;
    store_window_id = -1;
    selected_app = 0;
}

void AppStore::show() {
    if (store_visible) return;

    store_window_id = WindowManager::createWindow("SCos App Store", 8, 3, 64, 18);
    if (store_window_id >= 0) {
        store_visible = true;
        WindowManager::setActiveWindow(store_window_id);
        drawAppStore();
    }
}

void AppStore::hide() {
    if (!store_visible || store_window_id < 0) return;

    WindowManager::closeWindow(store_window_id);
    store_visible = false;
    store_window_id = -1;
}

bool AppStore::isVisible() {
    return store_visible;
}

void AppStore::drawAppStore() {
    if (!store_visible || store_window_id < 0) return;

    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int start_x = win->x + 2;
    int start_y = win->y + 2;

    // Clear content area
    for (int y = start_y; y < win->y + win->height - 1; ++y) {
        for (int x = start_x; x < win->x + win->width - 2; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x1F; // Blue background
        }
    }

    // Title
    const char* title = "Available Applications";
    for (int i = 0; title[i] && i < win->width - 4; ++i) {
        int idx = 2 * (start_y * 80 + start_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x1E; // Yellow
    }

    // Draw app list
    for (int i = 0; i < app_count && i < win->height - 6; ++i) {
        int app_y = start_y + 2 + i;
        uint8_t color = (i == selected_app) ? 0x4F : 0x1F; // Red if selected, blue otherwise

        // App name
        for (int j = 0; available_apps[i].name[j] && j < 20; ++j) {
            int idx = 2 * (app_y * 80 + start_x + j);
            video[idx] = available_apps[i].name[j];
            video[idx + 1] = color;
        }

        // Status
        const char* status = available_apps[i].installed ? "[INSTALLED]" : "[DOWNLOAD]";
        int status_x = start_x + 22;
        for (int j = 0; status[j] && j < 12; ++j) {
            int idx = 2 * (app_y * 80 + status_x + j);
            video[idx] = status[j];
            video[idx + 1] = available_apps[i].installed ? 0x2F : 0x6F; // Green if installed
        }

        // Version
        int ver_x = start_x + 36;
        for (int j = 0; available_apps[i].version[j] && j < 8; ++j) {
            int idx = 2 * (app_y * 80 + ver_x + j);
            video[idx] = available_apps[i].version[j];
            video[idx + 1] = color;
        }
    }

    // Instructions
    const char* instructions = "Use arrows to select, Enter to install/remove, Esc to exit";
    int instr_y = win->y + win->height - 2;
    for (int i = 0; instructions[i] && i < win->width - 4; ++i) {
        int idx = 2 * (instr_y * 80 + start_x + i);
        video[idx] = instructions[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void AppStore::handleInput(uint8_t key) {
    if (!store_visible) return;

    switch (key) {
        case 0x48: // Up arrow
            if (selected_app > 0) {
                selected_app--;
                drawAppStore();
            }
            break;
        case 0x50: // Down arrow
            if (selected_app < app_count - 1) {
                selected_app++;
                drawAppStore();
            }
            break;
        case 0x1C: // Enter
            toggleAppInstallation();
            break;
        case 0x01: // Escape
            hide();
            break;
    }
}

void AppStore::handleMouseClick(int x, int y) {
    if (!store_visible || store_window_id < 0) return;

    Window* win = WindowManager::getWindow(store_window_id);
    if (!win) return;

    // Check if click is within window
    if (x < win->x || x >= win->x + win->width || 
        y < win->y || y >= win->y + win->height) return;

    int start_x = win->x + 2;
    int start_y = win->y + 2;

    // Check if clicked on an app
    int rel_y = y - (start_y + 2);
    if (rel_y >= 0 && rel_y < app_count) {
        selected_app = rel_y;
        
        // Check if clicked on status button area
        int status_x = start_x + 22;
        if (x >= status_x && x < status_x + 12) {
            toggleAppInstallation();
        } else {
            drawAppStore();
        }
    }
}

void AppStore::toggleAppInstallation() {
    if (selected_app >= 0 && selected_app < app_count) {
        available_apps[selected_app].installed = !available_apps[selected_app].installed;
        drawAppStore();
    }
}