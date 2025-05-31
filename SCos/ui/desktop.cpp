
#include "desktop.hpp"
#include "app_launcher.hpp"
#include "window_manager.hpp"
#include "../apps/terminal.hpp"
#include "../apps/notepad.hpp"
#include "../apps/calculator.hpp"
#include "../apps/file_manager.hpp"
#include "../apps/calendar.hpp"
#include "../apps/settings.hpp"
#include "../apps/about.hpp"
#include "../apps/security_center.hpp"
#include "../apps/browser.hpp"
#include "../apps/app_store.hpp"
#include "../drivers/keyboard.hpp"

static bool desktop_initialized = false;
static bool running = true;

bool Desktop::init() {
    if (desktop_initialized) return true;
    
    // Initialize window manager
    WindowManager::init();
    WindowManager::clearScreen();
    
    // Initialize applications
    AppLauncher::init();
    Browser::init();
    AppStore::init();
    
    // Draw desktop
    drawDesktopBackground();
    drawTaskbar();
    
    desktop_initialized = true;
    return true;
}

void Desktop::drawDesktopBackground() {
    WindowManager::clearScreen();
    
    volatile char* video = (volatile char*)0xB8000;
    
    // Draw desktop background pattern
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x11; // Blue background
        }
    }
    
    // Draw desktop title
    const char* title = "SCos Desktop Environment";
    int title_x = (80 - 25) / 2;
    for (int i = 0; title[i]; ++i) {
        int idx = 2 * (2 * 80 + title_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x1F; // White on blue
    }
    
    // Draw welcome message
    const char* welcome = "Press Alt+Tab to open Application Launcher";
    int welcome_x = (80 - 43) / 2;
    for (int i = 0; welcome[i]; ++i) {
        int idx = 2 * (12 * 80 + welcome_x + i);
        video[idx] = welcome[i];
        video[idx + 1] = 0x17; // Grey on blue
    }
}

void Desktop::drawTaskbar() {
    volatile char* video = (volatile char*)0xB8000;
    int taskbar_y = 24;
    
    // Draw taskbar background
    for (int x = 0; x < 80; ++x) {
        int idx = 2 * (taskbar_y * 80 + x);
        video[idx] = ' ';
        video[idx + 1] = 0x70; // Black on white
    }
    
    // Draw start button
    const char* start_text = " SCos ";
    for (int i = 0; start_text[i]; ++i) {
        int idx = 2 * (taskbar_y * 80 + i);
        video[idx] = start_text[i];
        video[idx + 1] = 0x4F; // White on red
    }
    
    // Draw time (simplified)
    const char* time_text = "12:00";
    int time_x = 80 - 5;
    for (int i = 0; time_text[i]; ++i) {
        int idx = 2 * (taskbar_y * 80 + time_x + i);
        video[idx] = time_text[i];
        video[idx + 1] = 0x70; // Black on white
    }
}

void Desktop::launchApplication(AppType app) {
    switch (app) {
        case APP_TERMINAL:
            runTerminal();
            break;
        case APP_NOTEPAD:
            openNotepad("");
            break;
        case APP_CALCULATOR:
            launchCalculator();
            break;
        case APP_FILE_MANAGER:
            openFileManager();
            break;
        case APP_CALENDAR:
            openCalendar();
            break;
        case APP_SETTINGS:
            openSettings();
            break;
        case APP_ABOUT:
            openAbout();
            break;
        case APP_SECURITY:
            openSecurityCenter();
            break;
        case APP_BROWSER:
            openBrowser();
            break;
        case APP_APP_STORE:
            openAppStore();
            break;
    }
}

void Desktop::openBrowser() {
    Browser::show();
}

void Desktop::openAppStore() {
    AppStore::show();
}

void Desktop::runTerminal() {
    // Create terminal window
    int term_id = WindowManager::createWindow("Terminal", 10, 5, 60, 15);
    if (term_id >= 0) {
        WindowManager::setActiveWindow(term_id);
    }
}

void Desktop::openNotepad(const char* content) {
    // Create notepad window
    int notepad_id = WindowManager::createWindow("Notepad", 15, 3, 50, 18);
    if (notepad_id >= 0) {
        WindowManager::setActiveWindow(notepad_id);
    }
}

void Desktop::openFileManager() {
    // Create file manager window
    int fm_id = WindowManager::createWindow("File Manager", 8, 4, 64, 16);
    if (fm_id >= 0) {
        WindowManager::setActiveWindow(fm_id);
    }
}

void Desktop::openCalendar() {
    // Create calendar window
    int cal_id = WindowManager::createWindow("Calendar", 20, 6, 40, 14);
    if (cal_id >= 0) {
        WindowManager::setActiveWindow(cal_id);
    }
}

void Desktop::openSettings() {
    // Create settings window
    int set_id = WindowManager::createWindow("Settings", 12, 4, 56, 16);
    if (set_id >= 0) {
        WindowManager::setActiveWindow(set_id);
    }
}

void Desktop::openAbout() {
    // Create about window
    int about_id = WindowManager::createWindow("About SCos", 25, 8, 30, 10);
    if (about_id >= 0) {
        WindowManager::setActiveWindow(about_id);
    }
}

void Desktop::launchCalculator() {
    // Create calculator window
    int calc_id = WindowManager::createWindow("Calculator", 30, 10, 25, 15);
    if (calc_id >= 0) {
        WindowManager::setActiveWindow(calc_id);
    }
}

void Desktop::openSecurityCenter() {
    // Create security center window
    int sec_id = WindowManager::createWindow("Security Center", 5, 2, 70, 20);
    if (sec_id >= 0) {
        WindowManager::setActiveWindow(sec_id);
    }
}

void Desktop::handleInput() {
    uint8_t key = Keyboard::getLastKey();
    if (key == 0) return;
    
    // Check for Alt+Tab (application launcher)
    static bool alt_pressed = false;
    
    if (key == KEY_ALT) {
        alt_pressed = true;
        return;
    }
    
    if (alt_pressed && key == KEY_TAB) {
        if (AppLauncher::isVisible()) {
            AppLauncher::hideLauncher();
        } else {
            AppLauncher::showLauncher();
        }
        alt_pressed = false;
        return;
    }
    
    if (key != KEY_ALT) {
        alt_pressed = false;
    }
    
    // Pass input to active applications
    if (AppLauncher::isVisible()) {
        AppLauncher::handleInput(key);
    } else if (Browser::isVisible()) {
        Browser::handleInput(key);
    } else if (AppStore::isVisible()) {
        AppStore::handleInput(key);
    }
}

void Desktop::handle_events() {
    handleInput();
}

void Desktop::update() {
    updateDesktop();
}

void Desktop::updateDesktop() {
    // Refresh all windows
    WindowManager::refreshAll();
    
    // Update taskbar if needed
    drawTaskbar();
}

const char* Desktop::readFile(const char* path) {
    // Simplified file reading - would integrate with actual filesystem
    return "File content placeholder";
}
