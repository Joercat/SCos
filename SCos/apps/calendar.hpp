
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

// Legacy function declarations for backward compatibility
void openCalendar();
void openCalendarWithClock();
void openCalendarSimple();
void draw_mini_calendar(int x, int y);
void calendar_navigate_left();
void calendar_navigate_right();
void calendar_navigate_up();
void calendar_navigate_down();
void calendar_previous_month();
void calendar_next_month();
