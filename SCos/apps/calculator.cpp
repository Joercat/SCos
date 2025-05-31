#include "calculator.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>
#include <stdbool.h>

// Local math and string functions for freestanding environment
static int calc_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void calc_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void calc_strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static int calc_atoi(const char* str) {
    int result = 0;
    int sign = 1;
    int i = 0;

    if (str[0] == '-') {
        sign = -1;
        i = 1;
    }

    while (str[i] >= '0' && str[i] <= '9') {
        result = result * 10 + (str[i] - '0');
        i++;
    }

    return sign * result;
}

static void int_to_str(int num, char* str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    bool negative = false;
    if (num < 0) {
        negative = true;
        num = -num;
    }

    char temp[32];
    int i = 0;

    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }

    int j = 0;
    if (negative) {
        str[j++] = '-';
    }

    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
}

// Missing function implementations
static void sprintf(char* dest, const char* format, double value) {
    // Simple implementation for %.2f format
    int integer_part = (int)value;
    int decimal_part = (int)((value - integer_part) * 100);
    
    int_to_str(integer_part, dest);
    
    // Find end of string
    while (*dest) dest++;
    
    *dest++ = '.';
    if (decimal_part < 10) {
        *dest++ = '0';
    }
    int_to_str(decimal_part, dest);
}

static int strlen(const char* str) {
    return calc_strlen(str);
}

// VGA constants and functions
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define COLOR_BLACK 0
#define COLOR_WHITE 15
#define COLOR_LIGHT_GRAY 7
#define COLOR_BLUE 1

#define MAKE_COLOR(fg, bg) ((bg << 4) | fg)

void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile char* video = (volatile char*)0xB8000;
        int idx = 2 * (y * VGA_WIDTH + x);
        video[idx] = c;
        video[idx + 1] = color;
    }
}

void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < VGA_WIDTH; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

// Stub for getLastKey - this would normally come from keyboard driver
static uint8_t getLastKey() {
    // This is a stub - in a real OS this would interface with the keyboard driver
    return 0;
}

// Calculator state
static int calc_window_id = -1;
static bool calc_visible = false;
static char display[32] = "0";
static char current_number[32] = "";
static char operator_char = '\0';
static int stored_number = 0;
static bool new_number = true;

// Missing calculator variables
static double display_value = 0.0;
static double stored_value = 0.0;
static char current_operator = '\0';
static bool has_operand = false;
static bool just_calculated = false;

void drawCalculator() {
    uint8_t header_color = MAKE_COLOR(COLOR_WHITE, COLOR_BLUE);
    uint8_t text_color = MAKE_COLOR(COLOR_BLACK, COLOR_WHITE);
    uint8_t button_color = MAKE_COLOR(COLOR_BLACK, COLOR_LIGHT_GRAY);

    // Clear calculator area
    for (int y = 2; y < 20; y++) {
        for (int x = 2; x < 27; x++) {
            vga_put_char(x, y, ' ', text_color);
        }
    }

    vga_put_string(8, 3, "Calculator", header_color);

    // Draw display
    for (int x = 4; x < 25; x++) {
        vga_put_char(x, 5, ' ', MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));
    }

    // Display current number
    char display_str[32];
    sprintf(display_str, "%.2f", display_value);
    vga_put_string(24 - strlen(display_str), 5, display_str, MAKE_COLOR(COLOR_WHITE, COLOR_BLACK));

    // Draw buttons
    const char* buttons[4][4] = {
        {"7", "8", "9", "/"},
        {"4", "5", "6", "*"},
        {"1", "2", "3", "-"},
        {"0", ".", "=", "+"}
    };

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int x = 4 + col * 5;
            int y = 7 + row * 2;

            vga_put_char(x, y, '[', button_color);
            vga_put_char(x + 1, y, buttons[row][col][0], button_color);
            vga_put_char(x + 2, y, ']', button_color);
        }
    }

    // Instructions
    vga_put_string(4, 16, "Use number keys and operators", MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_WHITE));
    vga_put_string(4, 17, "Press Enter for result", MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_WHITE));
    vga_put_string(4, 18, "Press Esc to exit", MAKE_COLOR(COLOR_LIGHT_GRAY, COLOR_WHITE));
}

void inputDigit(int digit) {
    if (just_calculated) {
        display_value = 0;
        just_calculated = false;
    }
    display_value = display_value * 10 + digit;
}

// Helper function to convert char digit to int
static void inputDigitChar(char digit_char) {
    int digit = digit_char - '0';
    inputDigit(digit);
}

void inputOperator(char op) {
    if (has_operand && !just_calculated) {
        calculateResult();
    }
    stored_value = display_value;
    current_operator = op;
    has_operand = true;
    just_calculated = false;
    display_value = 0;
}

void calculateResult() {
    if (!has_operand) return;

    switch (current_operator) {
        case '+':
            display_value = stored_value + display_value;
            break;
        case '-':
            display_value = stored_value - display_value;
            break;
        case '*':
            display_value = stored_value * display_value;
            break;
        case '/':
            if (display_value != 0) {
                display_value = stored_value / display_value;
            }
            break;
    }

    has_operand = false;
    just_calculated = true;
}

void clearCalculator() {
    display_value = 0;
    stored_value = 0;
    current_operator = 0;
    has_operand = false;
    just_calculated = false;
}

void launchCalculator() {
    clearCalculator();
    drawCalculator();

    // Enter calculation loop
    uint8_t key;
    while (true) {
        key = getLastKey();
        if (key != 0) {
            if (key == 0x01) { // Escape
                break;
            }
            handleCalculatorInput(key);
            drawCalculator();
        }
    }
}

void closeCalculator() {
    if (!calc_visible || calc_window_id < 0) return;

    WindowManager::closeWindow(calc_window_id);
    calc_visible = false;
    calc_window_id = -1;
}

bool isCalculatorVisible() {
    return calc_visible;
}

void handleCalculatorInput(uint8_t key) {
    if (!calc_visible) return;

    switch (key) {
        case 0x01: // Escape
            closeCalculator();
            break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: // Number keys 1-0
            {
                char digit = (key == 0x0B) ? '0' : ('0' + key - 1);
                inputDigitChar(digit);
            }
            break;
        case 0x0D: // = (Enter)
            calculateResult();
            break;
        case 0x0C: // - 
            inputOperator('-');
            break;
        case 0x1A: // +
            inputOperator('+');
            break;
        case 0x35: // /
            inputOperator('/');
            break;
        case 0x37: // *
            inputOperator('*');
            break;
        case 0x0E: // Backspace (Clear)
            clearCalculator();
            break;
    }
}