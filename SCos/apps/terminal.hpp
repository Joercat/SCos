#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

#define MAX_TERMINAL_LINES 16
#define MAX_LINE_LENGTH 60

class Terminal {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawTerminal();
    static void processCommand(const char* command);
    static void addOutput(const char* text);
    static void clearScreen();
    static void scrollUp();
};