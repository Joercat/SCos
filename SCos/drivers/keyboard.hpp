#ifndef KEYBOARD_HPP
#define KEYBOARD_HPP

#include <stdint.h>
#include "../include/io_utils.h"

#define KEY_ESC 0x01
#define KEY_TAB 0x0F
#define KEY_ALT 0x38
#define KEY_ENTER 0x1C
#define KEY_BACKSPACE 0x0E

extern "C" void keyboard_handler();
bool init_keyboard();
char readScancode();

char getKey();
bool hasKey();
void handleKeyboardInterrupt();

extern "C" void keyboard_handler();

char getKey();
bool hasKey();
void clearKeyboardBuffer();
char waitForKey();

bool isShiftPressed();
bool isCtrlPressed();
bool isAltPressed();
bool isCapsLockOn();

bool isKeyboardBufferEmpty();
bool isKeyboardBufferFull();

namespace KeyboardNamespace {
    bool isPressed(char key);
    uint8_t getLastKey();
}

class Keyboard {
public:
    static bool hasKey() { return ::hasKey(); }
    static char getKey() { return ::getKey(); }
    static uint8_t getLastKey() { return KeyboardNamespace::getLastKey(); }
    static bool isPressed(char key) { return KeyboardNamespace::isPressed(key); }
};

#endif 
