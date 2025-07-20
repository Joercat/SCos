#ifndef KEYBOARD_HPP
#define KEYBOARD_HPP

#include <stdint.h>
#include "../include/io_utils.h"

// Key constants
#define KEY_ESC     0x01
#define KEY_TAB     0x0F
#define KEY_ALT     0x38
#define KEY_ENTER   0x1C
#define KEY_BACKSPACE 0x0E

// Core keyboard functions
extern "C" void keyboard_handler();
bool init_keyboard();
char readScancode();

// Public API
char getKey();
bool hasKey();
void handleKeyboardInterrupt();

// C wrapper
extern "C" void keyboard_handler();

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

// Forward declaration
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

#endif // KEYBOARD_HPP