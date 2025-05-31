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
  APP_CALCULATOR,
  APP_SECURITY,
  APP_BROWSER,
  APP_APP_STORE
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
  static void launchApplication(AppType app);

private:
  static void drawDesktopBackground();
  static void drawTaskbar();
  static void drawActiveApps();
  static void setupDefaultWindows();
  static void run();
  static void handleInput();
  static void updateDesktop();
  static void switchToNextWindow();
  static void closeActiveWindow();
  static void passInputToApplication(int window_id, uint8_t key);

  // Application launchers
  static void openNotepad(const char *content);
  static void runTerminal();
  static void openFileManager();
  static void openCalendar();
  static void openSettings();
  static void openAbout();
  static void launchCalculator();
  static void openSecurityCenter();
  static void openBrowser();
  static void openAppStore();

  // File system functions
  static const char *readFile(const char *path);
  
  
};

#endif