
#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

// Forward declaration of the openAbout function
void openAbout();

class About {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawAbout();
    static void drawSystemInfo();
    static void drawCredits();
    static void updateDisplay();
};
