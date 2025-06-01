#include <stdint.h>
#include "keyboard.hpp"
#include "../interrupt/idt.hpp"

// Keyboard buffer
#define KEYBOARD_BUFFER_SIZE 256
static char keyboardBuffer[KEYBOARD_BUFFER_SIZE];
static int bufferHead = 0;
static int bufferTail = 0;

// Modifier key states
static bool shiftPressed = false;
static bool ctrlPressed = false;
static bool altPressed = false;
static bool capsLock = false;

// Scancode to ASCII lookup table (US QWERTY layout)
static char scancodeTable[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shifted character lookup
static char shiftedTable[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char scancodeToChar(uint8_t scancode) {
    if (scancode >= 128) return 0;
    
    char c = scancodeTable[scancode];
    if (c == 0) return 0;
    
    // Apply shift or caps lock
    bool shouldShift = shiftPressed;
    if (c >= 'a' && c <= 'z' && capsLock) {
        shouldShift = !shouldShift;
    }
    
    if (shouldShift && scancode < 128) {
        char shifted = shiftedTable[scancode];
        if (shifted != 0) return shifted;
    }
    
    return c;
}

void addToKeyboardBuffer(char c) {
    if (!isKeyboardBufferFull()) {
        keyboardBuffer[bufferHead] = c;
        bufferHead = (bufferHead + 1) % KEYBOARD_BUFFER_SIZE;
    }
}

char getFromKeyboardBuffer() {
    if (isKeyboardBufferEmpty()) {
        return 0;
    }
    
    char c = keyboardBuffer[bufferTail];
    bufferTail = (bufferTail + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

// Keyboard buffer and state
#define KEYBOARD_BUFFER_SIZE 256
static char keyboardBuffer[KEYBOARD_BUFFER_SIZE];
static uint16_t bufferHead = 0;
static uint16_t bufferTail = 0;
static bool shiftPressed = false;
static bool ctrlPressed = false;
static bool altPressed = false;
static bool capsLock = false;

// Scancode to ASCII mapping tables
static const char scancodeToAscii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const char scancodeToAsciiShift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

// Special key scancodes
#define SCANCODE_LSHIFT     0x2A
#define SCANCODE_RSHIFT     0x36
#define SCANCODE_LCTRL      0x1D
#define SCANCODE_LALT       0x38
#define SCANCODE_CAPSLOCK   0x3A
#define SCANCODE_ESCAPE     0x01
#define SCANCODE_BACKSPACE  0x0E
#define SCANCODE_TAB        0x0F
#define SCANCODE_ENTER      0x1C
#define SCANCODE_SPACE      0x39

// Key release bit (bit 7 set means key release)
#define KEY_RELEASE         0x80

char readScancode() {
    uint8_t sc;
    asm volatile("inb %%dx, %%al" : "=a"(sc) : "d"(0x60));
    return sc;
}

bool isKeyboardBufferEmpty() {
    return bufferHead == bufferTail;
}

bool isKeyboardBufferFull() {
    return ((bufferHead + 1) % KEYBOARD_BUFFER_SIZE) == bufferTail;
}

void addToKeyboardBuffer(char key) {
    if (!isKeyboardBufferFull()) {
        keyboardBuffer[bufferHead] = key;
        bufferHead = (bufferHead + 1) % KEYBOARD_BUFFER_SIZE;
    }
}

char getFromKeyboardBuffer() {
    if (isKeyboardBufferEmpty()) {
        return 0;
    }

    char key = keyboardBuffer[bufferTail];
    bufferTail = (bufferTail + 1) % KEYBOARD_BUFFER_SIZE;
    return key;
}

bool Keyboard::isPressed(char key) {
    // Implementation would check current key state
    return false;
}

uint8_t Keyboard::getLastKey() {
    // Return the last pressed key scancode
    static uint8_t last_key = 0;
    if (hasKey()) {
        last_key = (uint8_t)getKey();
    }
    return last_key;
}

void handleSpecialKeys(uint8_t scancode, bool keyPressed) {
    switch (scancode) {
        case SCANCODE_LSHIFT:
        case SCANCODE_RSHIFT:
            shiftPressed = keyPressed;
            break;
        case SCANCODE_LCTRL:
            ctrlPressed = keyPressed;
            break;
        case SCANCODE_LALT:
            altPressed = keyPressed;
            break;
        case SCANCODE_CAPSLOCK:
            if (keyPressed) {
                capsLock = !capsLock;
            }
            break;
    }
}

char scancodeToChar(uint8_t scancode) {
    if (scancode >= 128) {
        return 0;
    }

    char ascii;
    bool useShift = shiftPressed;

    // Handle caps lock for letters
    if (scancode >= 0x10 && scancode <= 0x19) { // Q-P row
        useShift = shiftPressed ^ capsLock;
    } else if (scancode >= 0x1E && scancode <= 0x26) { // A-L row
        useShift = shiftPressed ^ capsLock;
    } else if (scancode >= 0x2C && scancode <= 0x32) { // Z-M row
        useShift = shiftPressed ^ capsLock;
    }

    if (useShift) {
        ascii = scancodeToAsciiShift[scancode];
    } else {
        ascii = scancodeToAscii[scancode];
    }

    return ascii;
}

extern "C" void keyboard_handler() {
    uint8_t scancode = readScancode();
    bool keyPressed = !(scancode & KEY_RELEASE);
    uint8_t actualScancode = scancode & ~KEY_RELEASE;

    // Handle modifier keys
    handleSpecialKeys(actualScancode, keyPressed);

    // Only process key press events for regular keys
    if (keyPressed) {
        char ascii = scancodeToChar(actualScancode);
        if (ascii != 0) {
            // Handle control key combinations
            if (ctrlPressed && ascii >= 'a' && ascii <= 'z') {
                ascii = ascii - 'a' + 1; // Convert to control character
            } else if (ctrlPressed && ascii >= 'A' && ascii <= 'Z') {
                ascii = ascii - 'A' + 1; // Convert to control character
            }

            addToKeyboardBuffer(ascii);
        }
    }

    // EOI is now handled in the assembly wrapper
}

bool init_keyboard() {
    // Initialize buffer pointers
    bufferHead = 0;
    bufferTail = 0;

    // Initialize modifier key states
    shiftPressed = false;
    ctrlPressed = false;
    altPressed = false;
    capsLock = false;

    // Clear keyboard buffer
    for (int i = 0; i < KEYBOARD_BUFFER_SIZE; i++) {
        keyboardBuffer[i] = 0;
    }

    // Basic keyboard initialization
    // The interrupt setup is now handled in idt.cpp

    return true;
}

// Public API functions
char getKey() {
    return getFromKeyboardBuffer();
}

bool hasKey() {
    return !isKeyboardBufferEmpty();
}

void handleKeyboardInterrupt() {
    uint8_t scancode = readScancode();
    
    // Check if this is a key release (bit 7 set)
    bool keyPressed = !(scancode & KEY_RELEASE);
    uint8_t actualScancode = scancode & 0x7F;
    
    // Handle modifier keys
    switch (actualScancode) {
        case SCANCODE_LSHIFT:
        case SCANCODE_RSHIFT:
            shiftPressed = keyPressed;
            return;
        case SCANCODE_LCTRL:
            ctrlPressed = keyPressed;
            return;
        case SCANCODE_LALT:
            altPressed = keyPressed;
            return;
        case SCANCODE_CAPSLOCK:
            if (keyPressed) {
                capsLock = !capsLock;
            }
            return;
    }
    
    // Only process key press events for regular keys
    if (keyPressed) {
        char ascii = scancodeToChar(actualScancode);
        if (ascii != 0) {
            // Handle control key combinations
            if (ctrlPressed && ascii >= 'a' && ascii <= 'z') {
                ascii = ascii - 'a' + 1; // Convert to control character
            } else if (ctrlPressed && ascii >= 'A' && ascii <= 'Z') {
                ascii = ascii - 'A' + 1; // Convert to control character
            }

            addToKeyboardBuffer(ascii);
        }
    }
}

// C wrapper for the keyboard handler
extern "C" void keyboard_handler() {
    handleKeyboardInterrupt();
}

void clearKeyboardBuffer() {
    bufferHead = bufferTail = 0;
}

// Modifier key state queries
bool isShiftPressed() {
    return shiftPressed;
}

bool isCtrlPressed() {
    return ctrlPressed;
}

bool isAltPressed() {
    return altPressed;
}

bool isCapsLockOn() {
    return capsLock;
}

// Wait for a key press (blocking)
char waitForKey() {
    while (isKeyboardBufferEmpty()) {
        // You might want to call a halt instruction here
        // or yield to other processes in a multitasking environment
        asm volatile("hlt");
    }
    return getFromKeyboardBuffer();
}