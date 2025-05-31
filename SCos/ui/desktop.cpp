
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
#include "../drivers/mouse.hpp"

static bool desktop_initialized = false;
static bool running = true;

bool Desktop::init() {
    if (desktop_initialized) return true;
    
    // Initialize theme manager first
    ThemeManager::init();
    
    // Initialize window manager
    WindowManager::init();
    WindowManager::clearScreen();
    
    // Initialize applications
    AppLauncher::init();
    Browser::init();
    AppStore::init();
    
    // Initialize mouse
    Mouse::init();
    
    // Draw desktop
    drawDesktopBackground();
    drawTaskbar();
    
    desktop_initialized = true;
    return true;
}

void Desktop::drawDesktopBackground() {
    WindowManager::clearScreen();
    
    // Use theme manager to draw background
    ThemeManager::drawCustomBackground();
    
    volatile char* video = (volatile char*)0xB8000;
    const Theme& theme = ThemeManager::getCurrentThemeData();
    
    // Draw desktop title
    const char* title = "SCos Desktop Environment";
    int title_x = (80 - 25) / 2;
    for (int i = 0; title[i]; ++i) {
        int idx = 2 * (2 * 80 + title_x + i);
        video[idx] = title[i];
        video[idx + 1] = theme.window_fg_color;
    }
    
    // Draw welcome message
    const char* welcome = "Press Alt+Tab to open Application Launcher, F1-F4 for themes";
    int welcome_x = (80 - 58) / 2;
    for (int i = 0; welcome[i] && i < 58; ++i) {
        int idx = 2 * (12 * 80 + welcome_x + i);
        video[idx] = welcome[i];
        video[idx + 1] = theme.foreground_color;
    }
}

void Desktop::drawTaskbar() {
    volatile char* video = (volatile char*)0xB8000;
    int taskbar_y = 24;
    const Theme& theme = ThemeManager::getCurrentThemeData();
    
    // Draw taskbar background
    for (int x = 0; x < 80; ++x) {
        int idx = 2 * (taskbar_y * 80 + x);
        video[idx] = ' ';
        video[idx + 1] = theme.taskbar_bg_color;
    }
    
    // Draw start button
    const char* start_text = " SCos ";
    for (int i = 0; start_text[i]; ++i) {
        int idx = 2 * (taskbar_y * 80 + i);
        video[idx] = start_text[i];
        video[idx + 1] = theme.selected_bg_color;
    }
    
    // Draw open application icons
    int app_start_x = 7;
    drawOpenAppIcons(app_start_x, taskbar_y);
    
    // Draw calendar date (simplified)
    const char* date_text = "Dec 15";
    int date_x = 80 - 12;
    for (int i = 0; date_text[i]; ++i) {
        int idx = 2 * (taskbar_y * 80 + date_x + i);
        video[idx] = date_text[i];
        video[idx + 1] = theme.foreground_color;
    }
    
    // Draw time (simplified)
    const char* time_text = "12:45";
    int time_x = 80 - 5;
    for (int i = 0; time_text[i]; ++i) {
        int idx = 2 * (taskbar_y * 80 + time_x + i);
        video[idx] = time_text[i];
        video[idx + 1] = theme.taskbar_fg_color;
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
    if (key != 0) {
        // Check for Alt+Tab (application launcher)
        static bool alt_pressed = false;
        
        if (key == KEY_ALT) {
            alt_pressed = true;
        } else if (alt_pressed && key == KEY_TAB) {
            if (AppLauncher::isVisible()) {
                AppLauncher::hideLauncher();
            } else {
                AppLauncher::showLauncher();
            }
            alt_pressed = false;
        } else if (key != KEY_ALT) {
            alt_pressed = false;
        }
        
        // Theme switching with function keys
        switch (key) {
            case 0x3B: // F1
                ThemeManager::setTheme(THEME_MATRIX_GREEN);
                drawDesktopBackground();
                drawTaskbar();
                break;
            case 0x3C: // F2
                ThemeManager::setTheme(THEME_MATRIX_RED);
                drawDesktopBackground();
                drawTaskbar();
                break;
            case 0x3D: // F3
                ThemeManager::setTheme(THEME_MATRIX_PURPLE);
                drawDesktopBackground();
                drawTaskbar();
                break;
            case 0x3E: // F4
                ThemeManager::setTheme(THEME_NATURE);
                drawDesktopBackground();
                drawTaskbar();
                break;
            case 0x3F: // F5
                ThemeManager::setTheme(THEME_DEFAULT_BLUE);
                drawDesktopBackground();
                drawTaskbar();
                break;
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
    
    // Handle mouse input
    handleMouseInput();
}

void Desktop::handle_events() {
    handleInput();
}

void Desktop::update() {
    updateDesktop();
}

void Desktop::drawOpenAppIcons(int start_x, int y) {
    volatile char* video = (volatile char*)0xB8000;
    int current_x = start_x;
    
    // Check which applications are currently open and draw their icons
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        Window* win = WindowManager::getWindow(i);
        if (win && win->visible && current_x < 60) { // Leave space for date/time
            // Draw app icon based on window title
            char icon = getAppIcon(win->title);
            int idx = 2 * (y * 80 + current_x);
            video[idx] = icon;
            video[idx + 1] = win->focused ? 0x1F : 0x17; // Blue if focused, grey otherwise
            
            current_x += 2; // Space between icons
        }
    }
}

char Desktop::getAppIcon(const char* title) {
    // Return appropriate icon based on application title
    if (title[0] == 'T' && title[1] == 'e') return 'T'; // Terminal
    if (title[0] == 'N' && title[1] == 'o') return 'N'; // Notepad
    if (title[0] == 'C' && title[1] == 'a' && title[2] == 'l' && title[3] == 'c') return 'C'; // Calculator
    if (title[0] == 'F' && title[1] == 'i') return 'F'; // File Manager
    if (title[0] == 'B' && title[1] == 'r') return 'B'; // Browser
    if (title[0] == 'A' && title[1] == 'p' && title[2] == 'p') return 'S'; // App Store
    if (title[0] == 'S' && title[1] == 'e') return 'G'; // Settings
    if (title[0] == 'A' && title[1] == 'b') return '?'; // About
    if (title[0] == 'C' && title[1] == 'a' && title[2] == 'l' && title[3] == 'e') return 'D'; // Calendar
    if (title[0] == 'S' && title[1] == 'e' && title[2] == 'c') return 'X'; // Security
    return '*'; // Default icon
}

void Desktop::handleMouseInput() {
    Mouse::update();
    
    if (Mouse::wasLeftButtonClicked()) {
        int mouse_x = Mouse::getX();
        int mouse_y = Mouse::getY();
        
        // Check if clicked on taskbar
        if (mouse_y == 24) {
            // Check if clicked on start button
            if (mouse_x >= 0 && mouse_x <= 6) {
                if (AppLauncher::isVisible()) {
                    AppLauncher::hideLauncher();
                } else {
                    AppLauncher::showLauncher();
                }
                return;
            }
            
            // Check if clicked on app icons in taskbar
            handleTaskbarClick(mouse_x);
            return;
        }
        
        // Pass mouse click to active applications
        if (AppLauncher::isVisible()) {
            AppLauncher::handleMouseClick(mouse_x, mouse_y);
        } else if (Browser::isVisible()) {
            Browser::handleMouseClick(mouse_x, mouse_y);
        } else if (AppStore::isVisible()) {
            AppStore::handleMouseClick(mouse_x, mouse_y);
        }
    }
}

void Desktop::handleTaskbarClick(int x) {
    // Handle clicks on taskbar app icons
    int current_x = 7;
    
    for (int i = 0; i < MAX_WINDOWS; ++i) {
        Window* win = WindowManager::getWindow(i);
        if (win && win->visible && current_x < 60) {
            if (x >= current_x && x < current_x + 2) {
                WindowManager::setActiveWindow(i);
                return;
            }
            current_x += 2;
        }
    }
}

void Desktop::updateDesktop() {
    // Refresh all windows
    WindowManager::refreshAll();
    
    // Update taskbar if needed
    drawTaskbar();
    
    // Update mouse
    Mouse::update();
}

const char* Desktop::readFile(const char* path) {
    // Simplified file reading - would integrate with actual filesystem
    return "File content placeholder";
}
