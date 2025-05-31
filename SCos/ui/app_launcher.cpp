#include "app_launcher.hpp"
#include "desktop.hpp"
#include "window_manager.hpp"
#include "../apps/terminal.hpp"
#include "../apps/notepad.hpp"
#include "../apps/calculator.hpp"
#include "../apps/file_manager.hpp"

// Local string function implementations for freestanding environment
static int custom_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static int custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

#define MAX_APPS 16
#define LAUNCHER_WIDTH 60
#define LAUNCHER_HEIGHT 20

static AppInfo registered_apps[MAX_APPS];
static int app_count = 0;
static int launcher_window_id = -1;
static bool launcher_visible = false;
static int selected_app = 0;

void AppLauncher::init() {
    app_count = 0;
    launcher_visible = false;
    selected_app = 0;
    
    // Register default applications
    registerApp("Terminal", "[T]", 45, 12, []() { Desktop::launchApplication(APP_TERMINAL); });
    registerApp("Notepad", "[N]", 50, 15, []() { Desktop::launchApplication(APP_NOTEPAD); });
    registerApp("File Manager", "[F]", 35, 18, []() { Desktop::launchApplication(APP_FILE_MANAGER); });
    registerApp("Calculator", "[C]", 25, 15, []() { Desktop::launchApplication(APP_CALCULATOR); });
    registerApp("Calendar", "[A]", 30, 16, []() { Desktop::launchApplication(APP_CALENDAR); });
    registerApp("Settings", "[S]", 40, 18, []() { Desktop::launchApplication(APP_SETTINGS); });
    registerApp("Security", "[X]", 50, 16, []() { Desktop::launchApplication(APP_SECURITY); });
    registerApp("About", "[?]", 35, 10, []() { Desktop::launchApplication(APP_ABOUT); });
}

void AppLauncher::registerApp(const char* name, const char* icon, 
                             int width, int height, void (*launch_func)()) {
    if (app_count >= MAX_APPS) return;
    
    AppInfo& app = registered_apps[app_count++];
    app.name = name;
    app.icon = icon;
    app.default_width = width;
    app.default_height = height;
    app.launch_func = launch_func;
}

void AppLauncher::showLauncher() {
    if (launcher_visible) return;
    
    launcher_window_id = WindowManager::createWindow("Application Launcher", 
                                                   10, 3, LAUNCHER_WIDTH, LAUNCHER_HEIGHT);
    if (launcher_window_id >= 0) {
        launcher_visible = true;
        WindowManager::setActiveWindow(launcher_window_id);
        drawLauncher();
    }
}

void AppLauncher::hideLauncher() {
    if (!launcher_visible || launcher_window_id < 0) return;
    
    WindowManager::closeWindow(launcher_window_id);
    launcher_visible = false;
    launcher_window_id = -1;
}

bool AppLauncher::isVisible() {
    return launcher_visible;
}

void AppLauncher::drawLauncher() {
    if (!launcher_visible || launcher_window_id < 0) return;
    
    Window* win = WindowManager::getWindow(launcher_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    
    // Draw launcher content
    int start_x = win->x + 2;
    int start_y = win->y + 2;
    
    // Title
    const char* title = "Select an application to launch:";
    for (int i = 0; title[i] && i < win->width - 4; ++i) {
        int idx = 2 * (start_y * 80 + start_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x1E; // Yellow
    }
    
    // Draw application list
    int apps_per_row = 4;
    int app_width = 12;
    int app_height = 3;
    
    for (int i = 0; i < app_count; ++i) {
        int row = i / apps_per_row;
        int col = i % apps_per_row;
        
        int app_x = start_x + col * app_width;
        int app_y = start_y + 3 + row * app_height;
        
        if (app_y + app_height >= win->y + win->height - 1) continue;
        
        drawAppIcon(app_x, app_y, registered_apps[i], i == selected_app);
    }
    
    // Instructions
    const char* instructions = "Use arrow keys, Enter to launch, Esc to close";
    int instr_y = win->y + win->height - 2;
    for (int i = 0; instructions[i] && i < win->width - 4; ++i) {
        int idx = 2 * (instr_y * 80 + start_x + i);
        video[idx] = instructions[i];
        video[idx + 1] = 0x17; // Grey
    }
}

void AppLauncher::drawAppIcon(int x, int y, const AppInfo& app, bool selected) {
    volatile char* video = (volatile char*)0xB8000;
    uint8_t color = selected ? 0x4F : 0x1F; // Red or blue background
    uint8_t text_color = selected ? 0x4E : 0x1E; // Text color
    
    // Draw icon background
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 10; ++i) {
            if (x + i >= 80 || y + j >= 25) continue;
            int idx = 2 * ((y + j) * 80 + (x + i));
            video[idx] = ' ';
            video[idx + 1] = color;
        }
    }
    
    // Draw icon
    int icon_len = custom_strlen(app.icon);
    for (int i = 0; i < icon_len && i < 10; ++i) {
        int idx = 2 * (y * 80 + x + i);
        video[idx] = app.icon[i];
        video[idx + 1] = text_color;
    }
    
    // Draw name
    int name_len = custom_strlen(app.name);
    for (int i = 0; i < name_len && i < 10; ++i) {
        int idx = 2 * ((y + 1) * 80 + x + i);
        video[idx] = app.name[i];
        video[idx + 1] = text_color;
    }
}

void AppLauncher::handleInput(uint8_t key) {
    if (!launcher_visible) return;
    
    switch (key) {
        case 0x4B: // Left arrow
            selectPrevApp();
            break;
        case 0x4D: // Right arrow
            selectNextApp();
            break;
        case 0x48: // Up arrow
            if (selected_app >= 4) {
                selected_app -= 4;
                updateSelection();
            }
            break;
        case 0x50: // Down arrow
            if (selected_app + 4 < app_count) {
                selected_app += 4;
                updateSelection();
            }
            break;
        case 0x1C: // Enter
            if (selected_app < app_count) {
                launchApp(selected_app);
                hideLauncher();
            }
            break;
        case 0x01: // Escape
            hideLauncher();
            break;
    }
}

void AppLauncher::selectNextApp() {
    if (app_count == 0) return;
    selected_app = (selected_app + 1) % app_count;
    updateSelection();
}

void AppLauncher::selectPrevApp() {
    if (app_count == 0) return;
    selected_app = (selected_app - 1 + app_count) % app_count;
    updateSelection();
}

void AppLauncher::updateSelection() {
    drawLauncher();
}

void AppLauncher::launchApp(int app_index) {
    if (app_index < 0 || app_index >= app_count) return;
    
    AppInfo& app = registered_apps[app_index];
    if (app.launch_func) {
        app.launch_func();
    }
}

void AppLauncher::launchAppByName(const char* name) {
    for (int i = 0; i < app_count; ++i) {
        if (custom_strcmp(registered_apps[i].name, name) == 0) {
            launchApp(i);
            return;
        }
    }
}