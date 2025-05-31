
#ifndef WINDOW_MANAGER_HPP
#define WINDOW_MANAGER_HPP

#include <stdint.h>

#define MAX_WINDOWS 10
#define MAX_TITLE_LENGTH 32

struct Window {
    int id;
    int x, y;
    int width, height;
    bool visible;
    bool focused;
    char title[MAX_TITLE_LENGTH];
};

class WindowManager {
public:
    static void init();
    static void clearScreen();
    static int createWindow(const char* title, int x, int y, int width, int height);
    static void drawWindow(int id);
    static void closeWindow(int id);
    static void moveWindow(int id, int x, int y);
    static void resizeWindow(int id, int width, int height);
    static void setActiveWindow(int id);
    static int getActiveWindow();
    static Window* getWindow(int id);
    static void refreshAll();
    
private:
    static void clearWindowArea(int x, int y, int width, int height);
};

// VGA text mode functions
void vga_clear_screen(uint8_t color);
void vga_clear_line(int y, uint8_t color);
void vga_put_char(int x, int y, char c, uint8_t color);
void vga_put_string(int x, int y, const char* str, uint8_t color);
void vga_draw_box(int x, int y, int width, int height, uint8_t color);
void center_text(int y, const char* text, uint8_t color);
void draw_horizontal_line(int y, int x1, int x2, char c, uint8_t color);

// Color constants
#define COLOR_BLACK         0
#define COLOR_BLUE          1
#define COLOR_GREEN         2
#define COLOR_CYAN          3
#define COLOR_RED           4
#define COLOR_MAGENTA       5
#define COLOR_BROWN         6
#define COLOR_LIGHT_GRAY    7
#define COLOR_DARK_GRAY     8
#define COLOR_LIGHT_BLUE    9
#define COLOR_LIGHT_GREEN   10
#define COLOR_LIGHT_CYAN    11
#define COLOR_LIGHT_RED     12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW        14
#define COLOR_WHITE         15

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

// Screen dimensions
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#endif
