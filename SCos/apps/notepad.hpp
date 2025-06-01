
#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

#define MAX_NOTEPAD_LINES 15
#define MAX_NOTEPAD_LINE_LENGTH 50

class Notepad {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawNotepad();
    static void insertChar(char c);
    static void deleteChar();
    static void newLine();
    static void moveCursor(int dx, int dy);
    static void updateDisplay();
};
