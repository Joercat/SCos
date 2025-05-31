#pragma once
#include <stdint.h>

#define MAX_DESKTOP_APPS 8

// Key codes
#define KEY_ALT     0x38
#define KEY_CTRL    0x1D
#define KEY_TAB     0x0F
#define KEY_ESC     0x01

enum AppType {
    APP_TERMINAL,
    APP_NOTEPAD,
    APP_CALENDAR,
    APP_SETTINGS,
    APP_ABOUT,
    APP_FILE_MANAGER
};

class Desktop {
public:
    // Main desktop functions
    static bool init();
    static void run();

    // Desktop rendering
    static void drawDesktopBackground();
    static void drawTaskbar();
    static void drawActiveApps();

    // Window management
    static void setupDefaultWindows();
    static void switchToNextWindow();
    static void launchApplication(AppType app);
    static void closeActiveWindow();

    // Input handling
    static void handleInput();
    static void passInputToApplication(int window_id, uint8_t key);
	static void handle_events();
    // Desktop updates
    static void updateDesktop();
	static void update();

private:
    // Internal state management
    static void refreshTaskbar();
};