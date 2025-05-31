#ifndef KEYBOARD_HPP
#define KEYBOARD_HPP

#include <stdint.h>

// Core keyboard functions
extern "C" void keyboard_handler();
bool init_keyboard();
char readScancode();

// Buffer management
char getKey();
bool hasKey();
void clearKeyboardBuffer();
char waitForKey();

// Modifier key state queries
bool isShiftPressed();
bool isCtrlPressed();
bool isAltPressed();
bool isCapsLockOn();

// Buffer state queries
bool isKeyboardBufferEmpty();
bool isKeyboardBufferFull();

class Keyboard {
public:
    static bool hasKey() { return ::hasKey(); }
    static char getKey() { return ::getKey(); }
    static bool isPressed(char key);
};

#endif // KEYBOARD_HPP