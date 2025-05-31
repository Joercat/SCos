#include "browser.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Local string function implementations for freestanding environment
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

// Browser state
static int browser_window_id = -1;
static bool browser_visible = false;
static char current_url[256] = "scos://home";
static char page_content[2048];

void Browser::init() {
    browser_visible = false;
    browser_window_id = -1;
    browser_strcpy(current_url, "scos://home");
    showHomePage();
}

void Browser::show() {
    if (browser_visible) return;

    browser_window_id = WindowManager::createWindow("SCos Browser", 5, 2, 70, 20);
    if (browser_window_id >= 0) {
        browser_visible = true;
        WindowManager::setActiveWindow(browser_window_id);
        showHomePage();
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

    drawNavigation();
    drawAddressBar();
    drawContent();
}

void Browser::drawNavigation() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int nav_y = win->y + 1;
    int start_x = win->x + 1;

    // Navigation buttons
    const char* nav_text = "[<] [>] [R] [H] [B]";
    for (int i = 0; nav_text[i] && i < win->width - 2; ++i) {
        int idx = 2 * (nav_y * 80 + start_x + i);
        video[idx] = nav_text[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void Browser::drawAddressBar() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int addr_y = win->y + 2;
    int start_x = win->x + 1;

    // Address bar background
    for (int i = 0; i < win->width - 2; ++i) {
        int idx = 2 * (addr_y * 80 + start_x + i);
        video[idx] = ' ';
        video[idx + 1] = 0x70; // Black on white
    }

    // URL text
    for (int i = 0; current_url[i] && i < win->width - 4; ++i) {
        int idx = 2 * (addr_y * 80 + start_x + 1 + i);
        video[idx] = current_url[i];
        video[idx + 1] = 0x70;
    }
}

void Browser::drawContent() {
    Window* win = WindowManager::getWindow(browser_window_id);
    if (!win) return;

    volatile char* video = (volatile char*)0xB8000;
    int content_start_y = win->y + 4;
    int start_x = win->x + 2;

    // Clear content area
    for (int y = content_start_y; y < win->y + win->height - 1; ++y) {
        for (int x = start_x; x < win->x + win->width - 2; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x1F; // Blue background
        }
    }

    // Draw page content
    int line = 0;
    int col = 0;
    for (int i = 0; page_content[i] && line < win->height - 6; ++i) {
        if (page_content[i] == '\n') {
            line++;
            col = 0;
        } else if (col < win->width - 6) {
            int idx = 2 * ((content_start_y + line) * 80 + (start_x + col));
            video[idx] = page_content[i];
            video[idx + 1] = 0x1E; // Yellow on blue
            col++;
        }
    }
}

void Browser::showHomePage() {
    browser_strcpy(current_url, "scos://home");

    const char* home_content = 
        "Welcome to SCos Browser!\n\n"
        "Quick Links:\n"
        "- scos://about - About this system\n"
        "- scos://apps - Application directory\n"
        "- scos://help - Help and documentation\n"
        "- scos://settings - System settings\n\n"
        "This is a simple text-based browser\n"
        "for the SCos operating system.";

    browser_strcpy(page_content, home_content);
}

void Browser::handleInput(uint8_t key) {
    if (!browser_visible) return;

    switch (key) {
        case 0x01: // Escape
            hide();
            break;
        case 0x23: // H key - Home
            showHomePage();
            drawBrowser();
            break;
        case 0x30: // B key - Back (simplified)
            showHomePage();
            drawBrowser();
            break;
        case 0x13: // R key - Refresh
            drawBrowser();
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

    // Simple click handling - could be expanded for buttons, links, etc.
    drawBrowser();
}

void Browser::navigateToUrl(const char* url) {
    // Simple URL navigation simulation
    store_strcpy(current_url, url);
    drawBrowser();
}