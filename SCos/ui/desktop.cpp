#include "desktop.hpp"
#include "window_manager.hpp"
#include "../apps/terminal.hpp"
#include "../apps/notepad.hpp"
#include "../apps/calendar.hpp"
#include "../apps/settings.hpp"
#include "../apps/about.hpp"
#include "../apps/file_manager.hpp"
#include "../fs/ramfs.hpp"
#include "../drivers/keyboard.hpp"

// Utility function implementations
int strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    
    for (const char* h = haystack; *h; h++) {
        const char* h_temp = h;
        const char* n_temp = needle;
        
        while (*h_temp && *n_temp && *h_temp == *n_temp) {
            h_temp++;
            n_temp++;
        }
        
        if (!*n_temp) return (char*)h;
    }
    return nullptr;
}

// Desktop state
static bool desktop_initialized = false;
static int taskbar_height = 2;
static int desktop_windows[MAX_DESKTOP_APPS];
static int desktop_window_count = 0;

bool Desktop::init() {
    if (desktop_initialized) return true;

    // Initialize window manager
    WindowManager::init();

    // Draw desktop background
    drawDesktopBackground();

    // Create taskbar
    drawTaskbar();

    // Create initial desktop windows
    setupDefaultWindows();

    desktop_initialized = true;

    // Start main desktop loop
    run();
    return true;
}

void Desktop::handle_events() {
    // Handle desktop events
}

void Desktop::update() {
    // Update desktop state
}

void Desktop::drawDesktopBackground() {
    WindowManager::clearScreen();

    // Draw desktop pattern or wallpaper
    volatile char* video = (volatile char*)0xB8000;
    for (int y = 0; y < 23; ++y) { // Leave space for taskbar
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            if ((x + y) % 4 == 0) {
                video[idx] = '.';
                video[idx + 1] = 0x08; // Dark grey
            } else {
                video[idx] = ' ';
                video[idx + 1] = 0x01; // Blue background
            }
        }
    }
}

void Desktop::drawTaskbar() {
    volatile char* video = (volatile char*)0xB8000;

    // Draw taskbar background
    for (int y = 23; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x70; // White on black
        }
    }

    // Draw SCos logo/button
    const char* os_name = "SCos";
    for (int i = 0; i < 4; ++i) {
        int idx = 2 * (23 * 80 + 2 + i);
        video[idx] = os_name[i];
        video[idx + 1] = 0x4F; // White on red
    }

    // Draw time (placeholder)
    const char* time_str = "12:34";
    for (int i = 0; i < 5; ++i) {
        int idx = 2 * (23 * 80 + 74 + i);
        video[idx] = time_str[i];
        video[idx + 1] = 0x70;
    }

    // Draw active application indicators
    drawActiveApps();
}

void Desktop::drawActiveApps() {
    volatile char* video = (volatile char*)0xB8000;
    int app_start = 10;

    for (int i = 0; i < desktop_window_count; ++i) {
        Window* win = WindowManager::getWindow(desktop_windows[i]);
        if (win && win->visible) {
            // Draw app indicator
            int pos = app_start + i * 8;
            if (pos < 70) {
                for (int j = 0; j < 6 && j < strlen(win->title); ++j) {
                    int idx = 2 * (24 * 80 + pos + j);
                    video[idx] = win->title[j];
                    video[idx + 1] = win->focused ? 0x1F : 0x70;
                }
            }
        }
    }
}

void Desktop::setupDefaultWindows() {
    // Create welcome notepad window
    int notepad_id = WindowManager::createWindow("Welcome - Notepad", 5, 3, 50, 15);
    if (notepad_id >= 0) {
        desktop_windows[desktop_window_count++] = notepad_id;
        openNotepad(readFile("home/welcome.txt"));
    }

    // Create terminal window
    int terminal_id = WindowManager::createWindow("Terminal", 20, 8, 45, 12);
    if (terminal_id >= 0) {
        desktop_windows[desktop_window_count++] = terminal_id;
        runTerminal();
    }

    // Create file manager
    int fm_id = WindowManager::createWindow("File Manager", 2, 2, 35, 18);
    if (fm_id >= 0) {
        desktop_windows[desktop_window_count++] = fm_id;
        openFileManager();
    }

    // Set terminal as active by default
    if (terminal_id >= 0) {
        WindowManager::setActiveWindow(terminal_id);
    }
}

void Desktop::run() {
    while (true) {
        handleInput();
        updateDesktop();

        // Simple delay loop
        for (volatile int i = 0; i < 100000; ++i) {}
    }
}

void Desktop::handleInput() {
    // Check for keyboard input
    if (Keyboard::hasKey()) {
        uint8_t key = Keyboard::getKey();

        // Handle desktop shortcuts
        if (key == KEY_ALT) {
            // Alt+Tab for window switching
            if (Keyboard::isPressed(KEY_TAB)) {
                switchToNextWindow();
            }
        } else if (key == KEY_CTRL) {
            // Ctrl+N for new terminal
            if (Keyboard::isPressed('n') || Keyboard::isPressed('N')) {
                launchApplication(APP_TERMINAL);
            }
            // Ctrl+Q to close active window
            else if (Keyboard::isPressed('q') || Keyboard::isPressed('Q')) {
                closeActiveWindow();
            }
        } else {
            // Pass input to active window/application
            int active = WindowManager::getActiveWindow();
            if (active >= 0) {
                passInputToApplication(active, key);
            }
        }
    }
}

void Desktop::updateDesktop() {
    // Refresh taskbar if needed
    static int update_counter = 0;
    if (++update_counter % 1000 == 0) {
        drawActiveApps();
    }
}

void Desktop::switchToNextWindow() {
    if (desktop_window_count <= 1) return;

    int current = WindowManager::getActiveWindow();
    int next_index = 0;

    // Find current window in desktop list
    for (int i = 0; i < desktop_window_count; ++i) {
        if (desktop_windows[i] == current) {
            next_index = (i + 1) % desktop_window_count;
            break;
        }
    }

    // Find next visible window
    for (int i = 0; i < desktop_window_count; ++i) {
        int try_index = (next_index + i) % desktop_window_count;
        Window* win = WindowManager::getWindow(desktop_windows[try_index]);
        if (win && win->visible) {
            WindowManager::setActiveWindow(desktop_windows[try_index]);
            break;
        }
    }
}

void Desktop::launchApplication(AppType app) {
    int window_id = -1;

    switch (app) {
        case APP_TERMINAL:
            window_id = WindowManager::createWindow("Terminal", 10 + (desktop_window_count * 3), 
                                                   5 + (desktop_window_count * 2), 45, 12);
            if (window_id >= 0) runTerminal();
            break;

        case APP_NOTEPAD:
            window_id = WindowManager::createWindow("Notepad", 15 + (desktop_window_count * 3), 
                                                   4 + (desktop_window_count * 2), 50, 15);
            if (window_id >= 0) openNotepad("");
            break;

        case APP_CALENDAR:
            window_id = WindowManager::createWindow("Calendar", 25, 6, 30, 16);
            if (window_id >= 0) openCalendar();
            break;

        case APP_SETTINGS:
            window_id = WindowManager::createWindow("Settings", 20, 5, 40, 18);
            if (window_id >= 0) openSettings();
            break;

        case APP_ABOUT:
            window_id = WindowManager::createWindow("About SCos", 30, 8, 35, 10);
            if (window_id >= 0) openAbout();
            break;

        case APP_FILE_MANAGER:
            window_id = WindowManager::createWindow("File Manager", 5, 3, 35, 18);
            if (window_id >= 0) openFileManager();
            break;
    }

    if (window_id >= 0 && desktop_window_count < MAX_DESKTOP_APPS) {
        desktop_windows[desktop_window_count++] = window_id;
        WindowManager::setActiveWindow(window_id);
    }
}

void Desktop::closeActiveWindow() {
    int active = WindowManager::getActiveWindow();
    if (active >= 0) {
        WindowManager::closeWindow(active);

        // Remove from desktop windows list
        for (int i = 0; i < desktop_window_count; ++i) {
            if (desktop_windows[i] == active) {
                for (int j = i; j < desktop_window_count - 1; ++j) {
                    desktop_windows[j] = desktop_windows[j + 1];
                }
                desktop_window_count--;
                break;
            }
        }

        drawTaskbar(); // Refresh taskbar
    }
}

void Desktop::passInputToApplication(int window_id, uint8_t key) {
    Window* win = WindowManager::getWindow(window_id);
    if (!win) return;

    // Route input based on window title/type
    if (strstr(win->title, "Terminal")) {
        Terminal::handleInput(key);
    } else if (strstr(win->title, "Notepad")) {
        Notepad::handleInput(key);
    } else if (strstr(win->title, "File Manager")) {
        FileManager::handleInput(key);
    }
    // Add more application input handlers as needed
}