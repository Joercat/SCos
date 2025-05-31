#ifndef NOTEPAD_HPP
#define NOTEPAD_HPP

#include <stdint.h>

class Notepad {
public:
    static void handleInput(uint8_t key);
};

// Function declarations for notepad application
void openNotepad(const char* content);
void closeNotepad();
bool isNotepadVisible();
void drawNotepadContent();
void handleNotepadInput(uint8_t key);

// VGA helper functions
void vga_put_char(int x, int y, char c, uint8_t color);
void vga_put_string(int x, int y, const char* str, uint8_t color);
void vga_clear_screen(uint8_t color);
void vga_clear_line(int y, uint8_t color);

// Color constants
#define COLOR_BLACK 0
#define COLOR_WHITE 15
#define COLOR_LIGHT_GRAY 7
#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

#endif