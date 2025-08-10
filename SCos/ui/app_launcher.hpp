#pragma once
#include "window_manager.hpp"

struct AppInfo {
    const char* name;
    const char* icon;
    int default_width;
    int default_height;
    void (*launch_func)();
};

class AppLauncher {
public:
    static void init();
    static void showLauncher();
    static void hideLauncher();
    static bool isVisible();
    
    
    static void registerApp(const char* name, const char* icon, 
                           int width, int height, void (*launch_func)());
    static void launchApp(int app_index);
    static void launchAppByName(const char* name);
    
    
    static void drawLauncher();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);
    static void selectNextApp();
    static void selectPrevApp();
    
private:
    static void drawAppIcon(int x, int y, const AppInfo& app, bool selected);
    static void updateSelection();
};
