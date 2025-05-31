#include "desktop.hpp"
#include "../apps/about.hpp"
#include "../apps/calendar.hpp"
#include "../apps/file_manager.hpp"
#include "../apps/notepad.hpp"
#include "../apps/settings.hpp"
#include "../apps/terminal.hpp"
#include "../drivers/keyboard.hpp"
#include "../fs/ramfs.hpp"
#include "../include/string.h"
#include "window_manager.hpp"

// Desktop state
static bool desktop_initialized = false;
// taskbar_height removed as it was unused
static int desktop_windows[MAX_DESKTOP_APPS];
static int desktop_window_count = 0;

bool Desktop::init() {
  if (desktop_initialized)
    return true;

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
  volatile char *video = (volatile char *)0xB8000;
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
  volatile char *video = (volatile char *)0xB8000;

  // Draw taskbar background
  for (int y = 23; y < 25; ++y) {
    for (int x = 0; x < 80; ++x) {
      int idx = 2 * (y * 80 + x);
      video[idx] = ' ';
      video[idx + 1] = 0x70; // White on black
    }
  }

  // Draw SCos logo/button
  const char *os_name = "SCos";
  for (int i = 0; i < 4; ++i) {
    int idx = 2 * (23 * 80 + 2 + i);
    video[idx] = os_name[i];
    video[idx + 1] = 0x4F; // White on red
  }

  // Draw time (placeholder)
  const char *time_str = "12:34";
  for (int i = 0; i < 5; ++i) {
    int idx = 2 * (23 * 80 + 74 + i);
    video[idx] = time_str[i];
    video[idx + 1] = 0x70;
  }

  // Draw active application indicators
  drawActiveApps();
}

void Desktop::drawActiveApps() {
  volatile char *video = (volatile char *)0xB8000;
  int app_start = 10;

  for (int i = 0; i < desktop_window_count; ++i) {
    Window *win = WindowManager::getWindow(desktop_windows[i]);
    if (win && win->visible) {
      // Draw app indicator
      int pos = app_start + i * 8;
      if (pos < 70) {
        int title_len = 0;
        while (win->title[title_len] && title_len < 80)
          title_len++;
        for (int j = 0; j < 6 && j < title_len; ++j) {
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
  int notepad_id =
      WindowManager::createWindow("Welcome - Notepad", 5, 3, 50, 15);
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
    for (volatile int i = 0; i < 100000; ++i) {
    }
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
  if (desktop_window_count <= 1)
    return;

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
    Window *win = WindowManager::getWindow(desktop_windows[try_index]);
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
    window_id =
        WindowManager::createWindow("Terminal", 10 + (desktop_window_count * 3),
                                    5 + (desktop_window_count * 2), 45, 12);
    if (window_id >= 0)
      runTerminal();
    break;

  case APP_NOTEPAD:
    window_id =
        WindowManager::createWindow("Notepad", 15 + (desktop_window_count * 3),
                                    4 + (desktop_window_count * 2), 50, 15);
    if (window_id >= 0)
      openNotepad("");
    break;

  case APP_CALENDAR:
    window_id = WindowManager::createWindow("Calendar", 25, 6, 30, 16);
    if (window_id >= 0)
      openCalendar();
    break;

  case APP_SETTINGS:
    window_id = WindowManager::createWindow("Settings", 20, 5, 40, 18);
    if (window_id >= 0)
      openSettings();
    break;

  case APP_ABOUT:
    window_id = WindowManager::createWindow("About SCos", 30, 8, 35, 10);
    if (window_id >= 0)
      openAbout();
    break;

  case APP_FILE_MANAGER:
    window_id = WindowManager::createWindow("File Manager", 5, 3, 35, 18);
    if (window_id >= 0)
      openFileManager();
    break;
  case APP_CALCULATOR:
    window_id = WindowManager::createWindow(
        "Calculator", 20 + (desktop_window_count * 3),
        7 + (desktop_window_count * 2), 40, 14);
    if (window_id >= 0)
      launchCalculator();
    break;
  }

  if (window_id >= 0 && desktop_window_count < MAX_DESKTOP_APPS) {
    desktop_windows[desktop_window_count++] = window_id;
    WindowManager::setActiveWindow(window_id);
  }
}

// Local string function implementations for freestanding environment
char *custom_strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;

  while (*haystack) {
    const char *h = haystack;
    const char *n = needle;

    while (*h && *n && (*h == *n)) {
      h++;
      n++;
    }

    if (!*n)
      return (char *)haystack;
    haystack++;
  }

  return nullptr;
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
  Window *win = WindowManager::getWindow(window_id);
  if (!win)
    return;

  // Route input based on window title/type
  if (custom_strstr(win->title, "Terminal")) {
    Terminal::handleInput(key);
  } else if (custom_strstr(win->title, "Notepad")) {
    Notepad::handleInput(key);
  } else if (custom_strstr(win->title, "File Manager")) {
    FileManager::handleInput(key);
  } else if (custom_strstr(win->title, "Calendar")) {
    Calendar::handleInput(key);
  }
  // Add more application input handlers as needed
}
// Application launcher functions
void Desktop::openNotepad(const char *content) {
  extern void openNotepad(const char *);
  openNotepad(content);
}

void Desktop::runTerminal() {
  extern void runTerminal();
  runTerminal();
}

void Desktop::openFileManager() {
  extern void openFileManager();
  openFileManager();
}

void Desktop::openCalendar() {
  extern void openCalendar();
  openCalendar();
}

void Desktop::openSettings() {
  extern void openSettings();
  openSettings();
}

void Desktop::openAbout() {
  extern void openAbout();
  openAbout();
}

void Desktop::launchCalculator() {
  extern void launchCalculator();
  launchCalculator();
}