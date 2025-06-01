#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

class AppStore {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);
    static void drawAppStore();
    static void toggleAppInstallation();
};