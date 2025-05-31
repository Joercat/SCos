
#include "calendar.hpp"
#include <stdint.h>

// VGA text mode constants
#define VGA_BUFFER ((volatile char*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BYTES_PER_CHAR 2

// Color attributes
#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x01
#define COLOR_GREEN 0x02
#define COLOR_CYAN 0x03
#define COLOR_RED 0x04
#define COLOR_MAGENTA 0x05
#define COLOR_BROWN 0x06
#define COLOR_LIGHT_GRAY 0x07
#define COLOR_DARK_GRAY 0x08
#define COLOR_LIGHT_BLUE 0x09
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_LIGHT_CYAN 0x0B
#define COLOR_LIGHT_RED 0x0C
#define COLOR_LIGHT_MAGENTA 0x0D
#define COLOR_YELLOW 0x0E
#define COLOR_WHITE 0x0F

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

// Date structure
typedef struct {
    int day;
    int month;
    int year;
    int day_of_week;  // 0=Sunday, 1=Monday, etc.
} Date;

// Calendar state
static Date current_date = {30, 5, 2025, 5}; // May 30, 2025 (Friday)
static Date view_date = {30, 5, 2025, 5};    // Currently viewed month
static int selected_day = 30;

// Month data
static const char* month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char* day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const int days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

// Simple events system
typedef struct {
    int day;
    int month;
    int year;
    char title[32];
    uint8_t color;
} Event;

static Event events[] = {
    {30, 5, 2025, "Today", COLOR_LIGHT_GREEN},
    {1, 6, 2025, "Summer Begin", COLOR_YELLOW},
    {4, 7, 2025, "Independence", COLOR_LIGHT_RED},
    {25, 12, 2025, "Christmas", COLOR_LIGHT_RED},
    {1, 1, 2025, "New Year", COLOR_LIGHT_CYAN},
    {14, 2, 2025, "Valentine", COLOR_LIGHT_MAGENTA}
};

#define NUM_EVENTS (sizeof(events) / sizeof(events[0]))

// Utility functions
static int calendar_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile char* pos = VGA_BUFFER + (y * VGA_WIDTH + x) * VGA_BYTES_PER_CHAR;
        pos[0] = c;
        pos[1] = color;
    }
}

static void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < VGA_WIDTH; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

static void vga_clear_screen(uint8_t color) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_put_char(x, y, ' ', color);
        }
    }
}

static void center_text(int y, const char* text, uint8_t color) {
    int len = calendar_strlen(text);
    int x = (VGA_WIDTH - len) / 2;
    vga_put_string(x, y, text, color);
}

static void int_to_string(int value, char* buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    int i = 0;
    int temp = value;
    
    while (temp > 0) {
        buffer[i++] = '0' + (temp % 10);
        temp /= 10;
    }
    buffer[i] = '\0';
    
    // Reverse the string
    for (int j = 0; j < i / 2; j++) {
        char tmp = buffer[j];
        buffer[j] = buffer[i - 1 - j];
        buffer[i - 1 - j] = tmp;
    }
}

// Date calculation functions
static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int get_days_in_month(int month, int year) {
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days_in_month[month - 1];
}

// Calculate day of week for the 1st of the month (0=Sunday)
static int get_first_day_of_month(int month, int year) {
    // Simple algorithm - in real OS you'd use a more sophisticated method
    // This is a simplified Zeller's congruence approximation
    int total_days = 0;
    
    // Count days from year 1 to target year
    for (int y = 1; y < year; y++) {
        total_days += is_leap_year(y) ? 366 : 365;
    }
    
    // Add days for months in target year
    for (int m = 1; m < month; m++) {
        total_days += get_days_in_month(m, year);
    }
    
    // January 1, year 1 was a Monday, so adjust
    return (total_days + 1) % 7;
}

static Event* find_event(int day, int month, int year) {
    for (unsigned int i = 0; i < NUM_EVENTS; i++) {
        if (events[i].day == day && events[i].month == month && events[i].year == year) {
            return &events[i];
        }
    }
    return 0; // NULL equivalent
}

// Calendar drawing functions
static void draw_calendar_header() {
    uint8_t header_color = MAKE_COLOR(COLOR_YELLOW, COLOR_BLUE);
    uint8_t title_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    
    // Month and year title
    char title[50];
    char year_str[8];
    int_to_string(view_date.year, year_str);
    
    // Build title string
    int pos = 0;
    const char* month_name = month_names[view_date.month - 1];
    for (int i = 0; month_name[i] && pos < 45; i++) {
        title[pos++] = month_name[i];
    }
    title[pos++] = ' ';
    for (int i = 0; year_str[i] && pos < 49; i++) {
        title[pos++] = year_str[i];
    }
    title[pos] = '\0';
    
    center_text(1, title, title_color);
    
    // Navigation hints
    vga_put_string(10, 1, "< Prev", header_color);
    vga_put_string(64, 1, "Next >", header_color);
    
    // Day headers
    int start_x = 15;
    for (int i = 0; i < 7; i++) {
        uint8_t day_color = (i == 0 || i == 6) ? MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK) : 
                                                 MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK);
        vga_put_string(start_x + i * 8, 3, day_names[i], day_color);
    }
    
    // Separator line
    for (int x = 10; x < 70; x++) {
        vga_put_char(x, 4, '-', MAKE_COLOR(COLOR_DARK_GRAY, COLOR_BLACK));
    }
}

static void draw_calendar_grid() {
    int days_in_current_month = get_days_in_month(view_date.month, view_date.year);
    int first_day = get_first_day_of_month(view_date.month, view_date.year);
    
    int start_x = 15;
    int start_y = 5;
    int current_day = 1;
    
    for (int week = 0; week < 6; week++) {
        for (int day_of_week = 0; day_of_week < 7; day_of_week++) {
            int x = start_x + day_of_week * 8;
            int y = start_y + week * 2;
            
            if (week == 0 && day_of_week < first_day) {
                // Empty cell before month starts
                vga_put_string(x, y, "  ", MAKE_COLOR(COLOR_DARK_GRAY, COLOR_BLACK));
            } else if (current_day <= days_in_current_month) {
                // Format day number
                char day_str[4];
                if (current_day < 10) {
                    day_str[0] = ' ';
                    day_str[1] = '0' + current_day;
                    day_str[2] = '\0';
                } else {
                    int_to_string(current_day, day_str);
                }
                
                // Determine color based on day type
                uint8_t day_color;
                Event* event = find_event(current_day, view_date.month, view_date.year);
                
                if (current_day == current_date.day && 
                    view_date.month == current_date.month && 
                    view_date.year == current_date.year) {
                    // Today
                    day_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GREEN);
                } else if (current_day == selected_day) {
                    // Selected day
                    day_color = MAKE_COLOR(COLOR_BLACK, COLOR_YELLOW);
                } else if (event) {
                    // Event day
                    day_color = MAKE_COLOR(event->color, COLOR_BLACK);
                } else if (day_of_week == 0 || day_of_week == 6) {
                    // Weekend
                    day_color = MAKE_COLOR(COLOR_LIGHT_RED, COLOR_BLACK);
                } else {
                    // Regular day
                    day_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLACK);
                }
                
                vga_put_string(x, y, day_str, day_color);
                
                // Event indicator
                if (event) {
                    vga_put_char(x + 2, y, '*', MAKE_COLOR(event->color, COLOR_BLACK));
                }
                
                current_day++;
            } else {
                // Empty cell after month ends
                vga_put_string(x, y, "  ", MAKE_COLOR(COLOR_DARK_GRAY, COLOR_BLACK));
            }
        }
    }
}

static void draw_sidebar() {
    uint8_t sidebar_color = MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK);
    
    // Current date info
    vga_put_string(2, 6, "Today:", sidebar_color);
    char today_str[15];
    char day_str[4];
    int_to_string(current_date.day, day_str);
    int pos = 0;
    
    // Build today string
    for (int i = 0; day_str[i] && pos < 10; i++) {
        today_str[pos++] = day_str[i];
    }
    today_str[pos++] = '/';
    char month_str[4];
    int_to_string(current_date.month, month_str);
    for (int i = 0; month_str[i] && pos < 14; i++) {
        today_str[pos++] = month_str[i];
    }
    today_str[pos] = '\0';
    
    vga_put_string(2, 7, today_str, MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    // Selected date info
    vga_put_string(2, 9, "Selected:", sidebar_color);
    Event* selected_event = find_event(selected_day, view_date.month, view_date.year);
    char selected_str[4];
    int_to_string(selected_day, selected_str);
    vga_put_string(2, 10, selected_str, MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    
    if (selected_event) {
        vga_put_string(2, 11, selected_event->title, MAKE_COLOR(selected_event->color, COLOR_BLACK));
    }
    
    // Upcoming events
    vga_put_string(2, 14, "Events:", sidebar_color);
    int event_line = 15;
    for (unsigned int i = 0; i < NUM_EVENTS && event_line < 22; i++) {
        if (events[i].month == view_date.month && events[i].year == view_date.year) {
            char event_str[12];
            char event_day_str[4];
            char event_day_str[12];
            int_to_string(events[i].day, event_day_str);
            
            int pos = 0;
            for (int j = 0; event_day_str[j] && pos < 3; j++) {
                event_str[pos++] = event_day_str[j];
            }
            event_str[pos++] = ':';
            event_str[pos++] = ' ';
            
            // Add first few chars of title
            for (int j = 0; events[i].title[j] && pos < 11; j++) {
                event_str[pos++] = events[i].title[j];
            }
            event_str[pos] = '\0';
            
            vga_put_string(2, event_line, event_str, MAKE_COLOR(events[i].color, COLOR_BLACK));
            event_line++;
        }
    }
}

static void draw_footer() {
    uint8_t footer_color = MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_BLACK);
    center_text(23, "Arrow Keys: Navigate | Space: Select | PgUp/PgDn: Change Month | ESC: Exit", footer_color);
}

// Main calendar function
void openCalendar() {
    // Clear screen with dark background
    vga_clear_screen(MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_BLACK));
    
    draw_calendar_header();
    draw_calendar_grid();
    draw_sidebar();
    draw_footer();
}

// Calendar with analog clock
void openCalendarWithClock() {
    openCalendar();
    
    // Add a simple ASCII clock in the corner
    uint8_t clock_color = MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK);
    vga_put_string(65, 6, "+-------+", clock_color);
    vga_put_string(65, 7, "|  12   |", clock_color);
    vga_put_string(65, 8, "| 9  3  |", clock_color);
    vga_put_string(65, 9, "|   6   |", clock_color);
    vga_put_string(65, 10, "+-------+", clock_color);
    
    // Current time (simplified - would get from RTC in real OS)
    vga_put_string(65, 12, "14:30:25", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    vga_put_string(65, 13, "Friday", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
}

// Mini calendar for embedding in other apps
void draw_mini_calendar(int x, int y) {
    uint8_t mini_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    uint8_t header_color = MAKE_COLOR(COLOR_YELLOW, COLOR_BLUE);
    
    // Header
    vga_put_string(x, y, "May 2025", header_color);
    vga_put_string(x, y + 1, "SMTWTFS", mini_color);
    
    // Simple grid (first few days)
    vga_put_string(x, y + 2, "    123", mini_color);
    vga_put_string(x, y + 3, "4567890", mini_color);
    vga_put_string(x, y + 4, "1112...", mini_color);
    
    // Highlight today
    vga_put_char(x + 6, y + 4, '0', MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GREEN));
}

// Navigation functions (called by keyboard handler)
void calendar_navigate_left() {
    if (selected_day > 1) {
        selected_day--;
        openCalendar();
    }
}

void calendar_navigate_right() {
    int max_day = get_days_in_month(view_date.month, view_date.year);
    if (selected_day < max_day) {
        selected_day++;
        openCalendar();
    }
}

void calendar_navigate_up() {
    if (selected_day > 7) {
        selected_day -= 7;
        openCalendar();
    }
}

void calendar_navigate_down() {
    int max_day = get_days_in_month(view_date.month, view_date.year);
    if (selected_day + 7 <= max_day) {
        selected_day += 7;
        openCalendar();
    }
}

void calendar_previous_month() {
    view_date.month--;
    if (view_date.month < 1) {
        view_date.month = 12;
        view_date.year--;
    }
    selected_day = 1;
    openCalendar();
}

void calendar_next_month() {
    view_date.month++;
    if (view_date.month > 12) {
        view_date.month = 1;
        view_date.year++;
    }
    selected_day = 1;
    openCalendar();
}

// Simple calendar (backward compatible)
void openCalendarSimple() {
    uint8_t bg_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLACK);
    vga_clear_screen(bg_color);
    
    center_text(10, "SCos Calendar", MAKE_COLOR(COLOR_YELLOW, COLOR_BLACK));
    center_text(12, "May 2025", MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    center_text(14, "Today: Friday, May 30", MAKE_COLOR(COLOR_LIGHT_GREEN, COLOR_BLACK));
    center_text(16, "Use openCalendar() for full interface", MAKE_COLOR(COLOR_LIGHT_CYAN, COLOR_BLACK));
}

// Calendar class implementation
void Calendar::handleInput(char key) {
    switch (key) {
        case 'w': // Up
        case 'W':
            calendar_navigate_up();
            break;
        case 's': // Down
        case 'S':
            calendar_navigate_down();
            break;
        case 'a': // Left
        case 'A':
            calendar_navigate_left();
            break;
        case 'd': // Right
        case 'D':
            calendar_navigate_right();
            break;
        case 'q': // Previous month
        case 'Q':
            calendar_previous_month();
            break;
        case 'e': // Next month
        case 'E':
            calendar_next_month();
            break;
        case ' ': // Select/enter
            // Could add event creation here
            break;
        case 27: // ESC
            // Exit calendar (handled by desktop)
            break;
        default:
            // Ignore other keys
            break;
    }
}
