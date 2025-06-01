
#include "browser.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Browser state
static int browser_window_id = -1;
static bool browser_visible = false;
static char current_url[256] = "scos://home";
static char address_bar[256] = "scos://home";

static int browser_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void browser_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

void Browser::init() {
    browser_visible = false;
    browser_window_id = -1;
    browser_strcpy(current_url, "scos://home");
    browser_strcpy(address_bar, "scos://home");
}

void Browser::show() {
    if (browser_visible) return;

    browser_window_id = WindowManager::createWindow("SCos Web Browser", 5, 2, 70, 20);
    if (browser_window_id >= 0) {
        browser_visible = true;
        WindowManager::setActiveWindow(browser_window_id);
        drawBrowser();
    }
}

void Browser::hide() {
    if (!browser_visible || browser_window_id < 0) return;

    WindowManager::closeWindow(browser_window_id);
    browser_visible = false;
    browser_window_id = -1;
}

bool Browser::isVisible() {
    return browser_visible;
}

void Browser::drawBrowser() {
    if (!browser_visible || browser_window_id < 0) return;

    Window* win = WindowManager::getWindow(browser_window_id);
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

    // Address bar
    const char* addr_label = "Address: ";
    for (int i = 0; addr_label[i] && i < 10; ++i) {
        int idx = 2 * (start_y * 80 + start_x + i);
        video[idx] = addr_label[i];
        video[idx + 1] = 0x1E; // Yellow
    }

    // Show URL
    for (int i = 0; address_bar[i] && i < 50; ++i) {
        int idx = 2 * (start_y * 80 + start_x + 9 + i);
        video[idx] = address_bar[i];
        video[idx + 1] = 0x1F; // White on blue
    }

    // Navigation buttons
    const char* back_btn = "[Back]";
    for (int i = 0; back_btn[i] && i < 6; ++i) {
        int idx = 2 * ((start_y + 2) * 80 + start_x + i);
        video[idx] = back_btn[i];
        video[idx + 1] = 0x2F; // Green
    }

    const char* fwd_btn = "[Forward]";
    for (int i = 0; fwd_btn[i] && i < 9; ++i) {
        int idx = 2 * ((start_y + 2) * 80 + start_x + 8 + i);
        video[idx] = fwd_btn[i];
        video[idx + 1] = 0x2F; // Green
    }

    const char* refresh_btn = "[Refresh]";
    for (int i = 0; refresh_btn[i] && i < 9; ++i) {
        int idx = 2 * ((start_y + 2) * 80 + start_x + 19 + i);
        video[idx] = refresh_btn[i];
        video[idx + 1] = 0x6F; // Cyan
    }

    // Content area
    const char* page_title = "Welcome to SCos Browser";
    for (int i = 0; page_title[i] && i < win->width - 4; ++i) {
        int idx = 2 * ((start_y + 4) * 80 + start_x + i);
        video[idx] = page_title[i];
        video[idx + 1] = 0x4F; // Red
    }

    // Sample content
    const char* content[] = {
        "This is the SCos internal browser.",
        "Supported protocols:",
        "- scos://home - Home page",
        "- scos://apps - Application directory",
        "- scos://settings - System settings",
        "- scos://help - Help documentation"
    };

    for (int i = 0; i < 6; ++i) {
        for (int j = 0; content[i][j] && j < win->width - 4; ++j) {
            int idx = 2 * ((start_y + 6 + i) * 80 + start_x + j);
            video[idx] = content[i][j];
            video[idx + 1] = 0x1F; // White on blue
        }
    }

    // Instructions
    const char* instructions = "Use Tab to navigate, Enter to activate, Esc to exit";
    int instr_y = win->y + win->height - 2;
    for (int i = 0; instructions[i] && i < win->width - 4; ++i) {
        int idx = 2 * (instr_y * 80 + start_x + i);
        video[idx] = instructions[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void Browser::handleInput(uint8_t key) {
    if (!browser_visible) return;

    switch (key) {
        case 0x01: // Escape
            hide();
            break;
        case 0x0F: // Tab
            // Navigate between elements
            break;
        case 0x1C: // Enter
            // Activate current element
            refreshPage();
            break;
        default:
            // Handle other keys
            break;
    }
}

void Browser::handleMouseClick(int x, int y) {
    if (!browser_visible || browser_window_id < 0) return;

    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;

    // Check if click is within window
    if (x < win->x || x >= win->x + win->width || 
        y < win->y || y >= win->y + win->height) return;

    // Handle navigation button clicks
    int start_x = win->x + 2;
    int start_y = win->y + 2;

    if (y == start_y + 2) {
        if (x >= start_x && x < start_x + 6) {
            // Back button clicked
            navigate("scos://home");
        } else if (x >= start_x + 8 && x < start_x + 17) {
            // Forward button clicked
            navigate("scos://apps");
        } else if (x >= start_x + 19 && x < start_x + 28) {
            // Refresh button clicked
            refreshPage();
        }
    }
}

void Browser::navigate(const char* url) {
    browser_strcpy(current_url, url);
    browser_strcpy(address_bar, url);
    drawBrowser();
}

void Browser::refreshPage() {
    drawBrowser();
}
