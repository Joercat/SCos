
#ifndef CALENDAR_HPP
#define CALENDAR_HPP

// Function declarations for calendar application
void openCalendar();
void openCalendarWithClock();
void openCalendarSimple();
void draw_mini_calendar(int x, int y);

// Navigation functions
void calendar_navigate_left();
void calendar_navigate_right();
void calendar_navigate_up();
void calendar_navigate_down();
void calendar_previous_month();
void calendar_next_month();

// Calendar class for input handling
class Calendar {
public:
    static void handleInput(char key);
};

#endif
