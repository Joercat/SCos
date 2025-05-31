#pragma once
#include <stdint.h>

class SecurityCenter {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void drawSecurityCenter();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);
};