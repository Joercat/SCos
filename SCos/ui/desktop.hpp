#ifndef DESKTOP_HPP
#define DESKTOP_HPP

#include "window_manager.hpp"
#include <stdint.h>

// Forward declarations for application classes
class Terminal;
class Notepad;
class FileManager;
class Keyboard;

#define MAX_DESKTOP_APPS 10

enum AppType {
    APP_TERMINAL,
    APP_NOTEPAD,
    APP_CALENDAR,
    APP_SETTINGS,
    APP_ABOUT,
    APP_FILE_MANAGER,
    APP_CALCULATOR
};

// Keyboard key constants
#define KEY_ALT 0x38
#define KEY_TAB 0x0F
#define KEY_CTRL 0x1D

class Desktop {
public:
    static bool init();
    static void handle_events();
    static void update();

private:
    static void drawDesktopBackground();
    static void drawTaskbar();
    static void drawActiveApps();
    static void setupDefaultWindows();
    static void run();
    static void handleInput();
    static void updateDesktop();
    static void switchToNextWindow();
    static void launchApplication(AppType app);
    static void closeActiveWindow();
    static void passInputToApplication(int window_id, uint8_t key);
    
    // Application launchers
    static void openNotepad(const char* content);
    static void runTerminal();
    static void openFileManager();
    static void openCalendar();
    static void openSettings();
    static void openAbout();
    static void launchCalculator();
    
    // File system functions
    static const char* readFile(const char* path);
};

// Utility functions
char* strstr(const char* haystack, const char* needle);

#endif