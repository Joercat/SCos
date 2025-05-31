#include <stdint.h>
#include <cstring>

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

// Settings structure
typedef struct {
    uint8_t theme_id;
    uint8_t brightness;
    uint8_t sound_enabled;
    uint8_t boot_timeout;
    uint8_t text_size;
    uint8_t cursor_blink;
} SystemSettings;

// Global settings (in real OS, this would be persistent storage)
static SystemSettings g_settings = {
    .theme_id = 2,        // Matrix Green theme
    .brightness = 80,
    .sound_enabled = 1,
    .boot_timeout = 5,
    .text_size = 1,
    .cursor_blink = 1
};

// Theme definitions
typedef struct {
    const char* name;
    uint8_t bg_color;
    uint8_t fg_color;
    uint8_t accent_color;
    uint8_t selected_color;
} Theme;

static const Theme themes[] = {
    {"Classic Blue", COLOR_BLUE, COLOR_WHITE, COLOR_YELLOW, COLOR_LIGHT_CYAN},
    {"Terminal Green", COLOR_BLACK, COLOR_GREEN, COLOR_LIGHT_GREEN, COLOR_YELLOW},
    {"Matrix Green", COLOR_BLACK, COLOR_LIGHT_GREEN, COLOR_GREEN, COLOR_WHITE},
    {"Retro Amber", COLOR_BLACK, COLOR_BROWN, COLOR_YELLOW, COLOR_WHITE},
    {"Ocean Blue", COLOR_BLUE, COLOR_LIGHT_CYAN, COLOR_WHITE, COLOR_YELLOW},
    {"Purple Haze", COLOR_MAGENTA, COLOR_WHITE, COLOR_LIGHT_MAGENTA, COLOR_YELLOW}
};

#define NUM_THEMES (sizeof(themes) / sizeof(themes[0]))

// Menu state
static int current_menu = 0;  // 0=main, 1=display, 2=system, 3=about
static int selected_item = 0;
static int in_submenu = 0;

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

static void vga_clear_screen(uint8_t color) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_put_char(x, y, ' ', color);
        }
    }
}

static void center_text(int y, const char* text, uint8_t color) {
    int len = strlen_custom(text);
    int x = (VGA_WIDTH - len) / 2;
    vga_put_string(x, y, text, color);
}

static void draw_horizontal_line(int y, int start_x, int end_x, char ch, uint8_t color) {
    for (int x = start_x; x <= end_x; x++) {
        vga_put_char(x, y, ch, color);
    }
}

static void draw_progress_bar(int x, int y, int width, int value, int max_value, uint8_t color) {
    int filled = (value * width) / max_value;

    vga_put_char(x, y, '[', color);
    for (int i = 1; i < width - 1; i++) {
        char ch = (i <= filled) ? '=' : ' ';
        vga_put_char(x + i, y, ch, color);
    }
    vga_put_char(x + width - 1, y, ']', color);
}

static void int_to_string(int value, char* buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    int i = 0;
    int temp = value;

    while (temp > 0) {
        buffer[i++] = '0' + (temp % 10);
        temp /= 10;
    }
    buffer[i] = '\0';

    // Reverse the string
    for (int j = 0; j < i / 2; j++) {
        char tmp = buffer[j];
        buffer[j] = buffer[i - 1 - j];
        buffer[i - 1 - j] = tmp;
    }
}

// Current theme colors
static uint8_t get_bg_color() { return themes[g_settings.theme_id].bg_color; }
static uint8_t get_fg_color() { return themes[g_settings.theme_id].fg_color; }
static uint8_t get_accent_color() { return themes[g_settings.theme_id].accent_color; }
static uint8_t get_selected_color() { return themes[g_settings.theme_id].selected_color; }

// Menu rendering functions
static void draw_header() {
    uint8_t header_color = MAKE_COLOR(get_accent_color(), get_bg_color());
    center_text(1, "SCos System Settings", header_color);
    draw_horizontal_line(2, 10, 69, '=', header_color);
}

static void draw_footer() {
    uint8_t footer_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    center_text(23, "Arrow Keys: Navigate | Enter: Select | ESC: Back/Exit", footer_color);
}

static void draw_main_menu() {
    const char* menu_items[] = {
        "Display Settings",
        "System Settings", 
        "Audio Settings",
        "Network Settings",
        "Security Settings",
        "About System"
    };

    int num_items = sizeof(menu_items) / sizeof(menu_items[0]);
    uint8_t normal_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    uint8_t selected_bg_color = MAKE_COLOR(get_selected_color(), get_accent_color());

    vga_put_string(5, 5, "Main Menu:", MAKE_COLOR(get_accent_color(), get_bg_color()));

    for (int i = 0; i < num_items; i++) {
        uint8_t color = (i == selected_item) ? selected_bg_color : normal_color;
        char prefix = (i == selected_item) ? '>' : ' ';

        vga_put_char(10, 7 + i, prefix, color);
        vga_put_string(12, 7 + i, menu_items[i], color);
    }
}

static void draw_display_settings() {
    uint8_t normal_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    uint8_t accent_color = MAKE_COLOR(get_accent_color(), get_bg_color());
    uint8_t selected_bg_color = MAKE_COLOR(get_selected_color(), get_accent_color());

    vga_put_string(5, 5, "Display Settings:", accent_color);

    // Theme selection
    uint8_t theme_color = (selected_item == 0) ? selected_bg_color : normal_color;
    vga_put_string(10, 7, "Theme:", theme_color);
    vga_put_string(25, 7, themes[g_settings.theme_id].name, theme_color);
    if (selected_item == 0) {
        vga_put_string(50, 7, "< >", accent_color);
    }

    // Brightness setting
    uint8_t bright_color = (selected_item == 1) ? selected_bg_color : normal_color;
    vga_put_string(10, 9, "Brightness:", bright_color);
    draw_progress_bar(25, 9, 20, g_settings.brightness, 100, bright_color);
    char bright_str[8];
    int_to_string(g_settings.brightness, bright_str);
    vga_put_string(47, 9, bright_str, bright_color);
    vga_put_string(50, 9, "%", bright_color);

    // Text size
    uint8_t size_color = (selected_item == 2) ? selected_bg_color : normal_color;
    vga_put_string(10, 11, "Text Size:", size_color);
    const char* sizes[] = {"Small", "Normal", "Large"};
    vga_put_string(25, 11, sizes[g_settings.text_size], size_color);
    if (selected_item == 2) {
        vga_put_string(35, 11, "< >", accent_color);
    }

    // Cursor blink
    uint8_t cursor_color = (selected_item == 3) ? selected_bg_color : normal_color;
    vga_put_string(10, 13, "Cursor Blink:", cursor_color);
    vga_put_string(25, 13, g_settings.cursor_blink ? "Enabled" : "Disabled", cursor_color);
    if (selected_item == 3) {
        vga_put_string(35, 13, "< >", accent_color);
    }

    // Theme preview
    vga_put_string(5, 16, "Theme Preview:", accent_color);
    for (int i = 0; i < 40; i++) {
        vga_put_char(10 + i, 17, ' ', MAKE_COLOR(get_fg_color(), get_bg_color()));
    }
    vga_put_string(12, 17, "Sample text in current theme", MAKE_COLOR(get_fg_color(), get_bg_color()));
    vga_put_string(12, 18, "Highlighted text example", MAKE_COLOR(get_selected_color(), get_accent_color()));
}

static void draw_system_settings() {
    uint8_t normal_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    uint8_t accent_color = MAKE_COLOR(get_accent_color(), get_bg_color());
    uint8_t selected_bg_color = MAKE_COLOR(get_selected_color(), get_accent_color());

    vga_put_string(5, 5, "System Settings:", accent_color);

    // Boot timeout
    uint8_t boot_color = (selected_item == 0) ? selected_bg_color : normal_color;
    vga_put_string(10, 7, "Boot Timeout:", boot_color);
    char timeout_str[8];
    int_to_string(g_settings.boot_timeout, timeout_str);
    vga_put_string(25, 7, timeout_str, boot_color);
    vga_put_string(27, 7, " seconds", boot_color);
    if (selected_item == 0) {
        vga_put_string(38, 7, "< >", accent_color);
    }

    // Sound
    uint8_t sound_color = (selected_item == 1) ? selected_bg_color : normal_color;
    vga_put_string(10, 9, "System Sounds:", sound_color);
    vga_put_string(25, 9, g_settings.sound_enabled ? "Enabled" : "Disabled", sound_color);
    if (selected_item == 1) {
        vga_put_string(35, 9, "< >", accent_color);
    }

    // System info
    vga_put_string(5, 12, "System Information:", accent_color);
    vga_put_string(10, 14, "OS Version: SCos v1.3.0", normal_color);
    vga_put_string(10, 15, "Kernel: Monolithic", normal_color);
    vga_put_string(10, 16, "Architecture: x86 32-bit", normal_color);
    vga_put_string(10, 17, "Memory: 32 MB", normal_color);

    // Actions
    uint8_t restart_color = (selected_item == 2) ? selected_bg_color : normal_color;
    uint8_t reset_color = (selected_item == 3) ? selected_bg_color : normal_color;

    vga_put_string(10, 19, "Restart System", restart_color);
    vga_put_string(10, 20, "Reset to Defaults", reset_color);
}

// Main settings interface
void openSettings() {
    vga_clear_screen(MAKE_COLOR(get_fg_color(), get_bg_color()));

    draw_header();

    switch (current_menu) {
        case 0:
            draw_main_menu();
            break;
        case 1:
            draw_display_settings();
            break;
        case 2:
            draw_system_settings();
            break;
    }

    draw_footer();
}

// Navigation functions (would be called by keyboard handler)
void settings_navigate_up() {
    if (selected_item > 0) {
        selected_item--;
        openSettings(); // Refresh display
    }
}

void settings_navigate_down() {
    int max_items = 6; // Default for main menu
    if (current_menu == 1) max_items = 4; // Display settings
    if (current_menu == 2) max_items = 4; // System settings

    if (selected_item < max_items - 1) {
        selected_item++;
        openSettings(); // Refresh display
    }
}

void settings_select() {
    if (current_menu == 0) {
        // Main menu selection
        current_menu = selected_item + 1;
        selected_item = 0;
        if (current_menu <= 2) { // Only display and system implemented
            openSettings();
        } else {
            current_menu = 0; // Go back for unimplemented menus
        }
    } else {
        // Handle submenu selections
        if (current_menu == 1) { // Display settings
            switch (selected_item) {
                case 0: // Theme
                    g_settings.theme_id = (g_settings.theme_id + 1) % NUM_THEMES;
                    break;
                case 1: // Brightness
                    g_settings.brightness = (g_settings.brightness + 10) % 101;
                    if (g_settings.brightness == 0) g_settings.brightness = 10;
                    break;
                case 2: // Text size
                    g_settings.text_size = (g_settings.text_size + 1) % 3;
                    break;
                case 3: // Cursor blink
                    g_settings.cursor_blink = !g_settings.cursor_blink;
                    break;
            }
            openSettings();
        }
    }
}

void settings_back() {
    if (current_menu == 0) {
        // Exit settings entirely
        vga_clear_screen(MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_BLACK));
        center_text(12, "Returning to desktop...", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    } else {
        // Go back to main menu
        current_menu = 0;
        selected_item = 0;
        openSettings();
    }
}

// Simple settings app (backward compatible)
void openSettingsSimple() {
    uint8_t bg_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    vga_clear_screen(bg_color);

    uint8_t title_color = MAKE_COLOR(get_accent_color(), get_bg_color());
    center_text(8, "SCos Settings", title_color);
    center_text(9, "=============", title_color);

    uint8_t info_color = MAKE_COLOR(get_fg_color(), get_bg_color());
    char theme_line[50] = "Current Theme: ";
    // Simple string concatenation
    const char* theme_name = themes[g_settings.theme_id].name;
    int pos = 15;
    for (int i = 0; theme_name[i] && pos < 49; i++) {
        theme_line[pos++] = theme_name[i];
    }
    theme_line[pos] = '\0';

    center_text(12, theme_line, info_color);
    center_text(14, "Use openSettings() for full interface", MAKE_COLOR(get_accent_color(), get_bg_color()));
}
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

void openSettings() {
    uint8_t header_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    uint8_t text_color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);

    // Clear area and draw settings
    for (int y = 5; y < 22; y++) {
        for (int x = 20; x < 60; x++) {
            vga_put_char(x, y, ' ', text_color);
        }
    }

    vga_put_string(30, 6, "SCos Settings", header_color);
    vga_put_string(22, 8, "Display Settings:", text_color);
    vga_put_string(24, 9, "[ ] Dark Mode", text_color);
    vga_put_string(24, 10, "[X] Show Taskbar", text_color);
    vga_put_string(24, 11, "[X] Desktop Icons", text_color);

    vga_put_string(22, 13, "System Settings:", text_color);
    vga_put_string(24, 14, "Memory: 16MB Available", text_color);
    vga_put_string(24, 15, "CPU: x86 Compatible", text_color);

    vga_put_string(22, 17, "Sound Settings:", text_color);
    vga_put_string(24, 18, "[ ] System Beep", text_color);
    vga_put_string(24, 19, "Volume: [====------]", text_color);
}

void Settings::handleInput(char key) {
    // Handle settings input
}
// desktop.hpp
#ifndef DESKTOP_HPP
#define DESKTOP_HPP

#include <stdint.h>

class Window; // Forward declaration

class Desktop {
public:
    Desktop(int width, int height);
    ~Desktop();
    void addWindow(Window* window);
    void removeWindow(Window* window);
    void draw();
    void handleInput(int key);

private:
    int width_;
    int height_;
    Window** windows_;
    int numWindows_;
    int capacity_;
};

#endif
// desktop.cpp
#include "../ui/desktop.hpp"
#include "window.hpp"
#include <iostream>
#include <cstring> // Include for strstr
#include <vector>

Desktop::Desktop(int width, int height) : width_(width), height_(height), numWindows_(0), capacity_(5) {
    windows_ = new Window*[capacity_];
    for (int i = 0; i < capacity_; ++i) {
        windows_[i] = nullptr;
    }
}

Desktop::~Desktop() {
    for (int i = 0; i < numWindows_; ++i) {
        delete windows_[i];
    }
    delete[] windows_;
}

void Desktop::addWindow(Window* window) {
    if (numWindows_ == capacity_) {
        // Resize the array if it's full
        int newCapacity = capacity_ * 2;
        Window** newWindows = new Window*[newCapacity];
        for (int i = 0; i < numWindows_; ++i) {
            newWindows[i] = windows_[i];
        }
        delete[] windows_;
        windows_ = newWindows;
        capacity_ = newCapacity;
    }
    windows_[numWindows_++] = window;
}

void Desktop::removeWindow(Window* window) {
    int index = -1;
    for (int i = 0; i < numWindows_; ++i) {
        if (windows_[i] == window) {
            index = i;
            break;
        }
    }
    if (index != -1) {
        delete windows_[index];
        for (int i = index; i < numWindows_ - 1; ++i) {
            windows_[i] = windows_[i + 1];
        }
        windows_[numWindows_ - 1] = nullptr;
        numWindows_--;
    }
}

void Desktop::draw() {
    // Clear the screen (implementation depends on your environment)
    std::cout << "\033[2J\033[H"; // Clear screen for terminal
    std::cout << "Desktop: " << width_ << "x" << height_ << std::endl;
    for (int i = 0; i < numWindows_; ++i) {
        windows_[i]->draw();
    }
}

void Desktop::handleInput(int key) {
    std::cout << "Desktop Handle Input: " << key << std::endl;
    // You can implement key handling logic here, such as moving focus between windows
}
// window_manager.hpp
#ifndef WINDOW_MANAGER_HPP
#define WINDOW_MANAGER_HPP

#include "window.hpp"
#include "../ui/desktop.hpp"

class WindowManager {
public:
    WindowManager(int desktopWidth, int desktopHeight);
    ~WindowManager();
    Window* createWindow(int x, int y, int width, int height, const char* title);
    void destroyWindow(Window* window);
    void draw();
    void handleInput(int key);

private:
    Desktop* desktop_;
};

#endif
// window_manager.cpp
#include "window_manager.hpp"
#include <cstring> // Include for strlen
#include <iostream>

WindowManager::WindowManager(int desktopWidth, int desktopHeight) {
    desktop_ = new Desktop(desktopWidth, desktopHeight);
}

WindowManager::~WindowManager() {
    delete desktop_;
}

Window* WindowManager::createWindow(int x, int y, int width, int height, const char* title) {
    Window* window = new Window(x, y, width, height, title);
    desktop_->addWindow(window);
    return window;
}

void WindowManager::destroyWindow(Window* window) {
    desktop_->removeWindow(window);
}

void WindowManager::draw() {
    desktop_->draw();
}

void WindowManager::handleInput(int key) {
    desktop_->handleInput(key);
}
// terminal.hpp
#ifndef TERMINAL_HPP
#define TERMINAL_HPP

#include "window.hpp"

class Terminal : public Window {
public:
    Terminal(int x, int y, int width, int height, const char* title);
    void executeCommand(const char* command);
    void draw() override;
};

#endif
// terminal.cpp
#include "terminal.hpp"
#include <iostream>
#include <string> // Include for string class
#include <cstring> // Include for string functions like strcpy
#include <vector>

Terminal::Terminal(int x, int y, int width, int height, const char* title)
    : Window(x, y, width, height, title) {}

void Terminal::executeCommand(const char* command) {
    std::cout << "Executing command: " << command << std::endl;
    // Implement command execution logic here
}

void Terminal::draw() override {
    Window::draw();
    std::cout << "Drawing Terminal Content" << std::endl;
    // Implement terminal drawing logic here
}
// settings.hpp
#ifndef SETTINGS_HPP
#define SETTINGS_HPP

class Settings {
public:
    void openSettings();
    void handleInput(char key);
};

#endif
// settings.cpp
#include "settings.hpp"
#include "../ui/desktop.hpp"

// Local string function implementations for freestanding environment
static int settings_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}
void Settings::openSettings() {
    // Implement settings display logic here - no std::cout in freestanding environment
}

void Settings::handleInput(char key) {
    // Implement settings input handling logic here - no std::cout in freestanding environment
}
// about.hpp
#ifndef ABOUT_HPP
#define ABOUT_HPP

class About {
public:
    void openAbout();
};

#endif
// about.cpp
#include "about.hpp"
#include <iostream> // Add missing include
#include <cstring>
void About::openAbout() {
    // Implement about display logic here - no std::cout in freestanding environment
}
// main.cpp
#include "window_manager.hpp"
#include "terminal.hpp"
#include "settings.hpp"
#include "about.hpp"

int main() {
    WindowManager wm(80, 25); // VGA text mode dimensions
    Window* window1 = wm.createWindow(5, 5, 30, 10, "Window 1");
    Terminal* terminal1 = new Terminal(10, 10, 40, 12, "Terminal 1");
    wm.desktop_->addWindow(terminal1);

    Settings settings;
    About about;

    wm.draw();

    settings.openSettings();
    about.openAbout();

    wm.handleInput('a');
    terminal1->executeCommand("ls -l");

    wm.destroyWindow(window1);
    wm.destroyWindow(terminal1);

    return 0;
}