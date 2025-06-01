
#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

class Calendar {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawCalendar();
    static void drawMonth(int month, int year);
    static void navigateMonth(int direction);
    static void selectDate(int day);
    static void updateDisplay();
};
