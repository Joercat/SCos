
#include "calculator.hpp"
#include "../ui/window_manager.hpp"
#include <stdint.h>

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

// Calculator state
static int calc_window_id = -1;
static bool calc_visible = false;
static char display[32] = "0";
static char current_number[32] = "";
static char operator_char = '\0';
static int stored_number = 0;
static bool new_number = true;

void launchCalculator() {
    if (calc_visible) return;
    
    // Initialize calculator
    calc_strcpy(display, "0");
    calc_strcpy(current_number, "");
    operator_char = '\0';
    stored_number = 0;
    new_number = true;
    
    // Create calculator window
    calc_window_id = WindowManager::createWindow("Calculator", 30, 10, 25, 15);
    if (calc_window_id >= 0) {
        calc_visible = true;
        WindowManager::setActiveWindow(calc_window_id);
        drawCalculator();
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

void drawCalculator() {
    if (!calc_visible || calc_window_id < 0) return;
    
    Window* win = WindowManager::getWindow(calc_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    int start_x = win->x + 2;
    int start_y = win->y + 2;
    
    // Clear content area
    for (int y = start_y; y < win->y + win->height - 1; ++y) {
        for (int x = start_x; x < win->x + win->width - 2; ++x) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x1F; // Blue background
        }
    }
    
    // Draw display
    int display_y = start_y;
    for (int i = 0; i < win->width - 6; ++i) {
        int idx = 2 * (display_y * 80 + start_x + i);
        video[idx] = ' ';
        video[idx + 1] = 0x70; // Black on white
    }
    
    // Display number (right-aligned)
    int display_len = calc_strlen(display);
    int display_start = start_x + win->width - 6 - display_len;
    for (int i = 0; display[i]; ++i) {
        int idx = 2 * (display_y * 80 + display_start + i);
        video[idx] = display[i];
        video[idx + 1] = 0x70;
    }
    
    // Draw button layout
    const char* buttons[] = {
        "C", "+/-", "%", "/",
        "7", "8", "9", "*",
        "4", "5", "6", "-",
        "1", "2", "3", "+",
        "0", ".", "=", ""
    };
    
    int button_y = start_y + 2;
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 4 && buttons[row * 4 + col][0]; ++col) {
            int btn_x = start_x + col * 4;
            int btn_y = button_y + row;
            
            if (btn_y < win->y + win->height - 1) {
                // Button background
                for (int i = 0; i < 3; ++i) {
                    int idx = 2 * (btn_y * 80 + btn_x + i);
                    video[idx] = ' ';
                    video[idx + 1] = 0x17; // Grey
                }
                
                // Button text
                const char* btn_text = buttons[row * 4 + col];
                int idx = 2 * (btn_y * 80 + btn_x + 1);
                video[idx] = btn_text[0];
                video[idx + 1] = 0x17;
            }
        }
    }
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
                inputDigit(digit);
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

void inputDigit(char digit) {
    if (new_number) {
        calc_strcpy(display, "");
        new_number = false;
    }
    
    if (calc_strlen(display) < 15) {
        if (display[0] == '0' && calc_strlen(display) == 1) {
            display[0] = digit;
        } else {
            char temp[2] = {digit, '\0'};
            calc_strcat(display, temp);
        }
        drawCalculator();
    }
}

void inputOperator(char op) {
    if (operator_char != '\0') {
        calculateResult();
    }
    
    stored_number = calc_atoi(display);
    operator_char = op;
    new_number = true;
}

void calculateResult() {
    if (operator_char == '\0') return;
    
    int current = calc_atoi(display);
    int result = stored_number;
    
    switch (operator_char) {
        case '+':
            result = stored_number + current;
            break;
        case '-':
            result = stored_number - current;
            break;
        case '*':
            result = stored_number * current;
            break;
        case '/':
            if (current != 0) {
                result = stored_number / current;
            } else {
                calc_strcpy(display, "Error");
                operator_char = '\0';
                new_number = true;
                drawCalculator();
                return;
            }
            break;
    }
    
    int_to_str(result, display);
    operator_char = '\0';
    new_number = true;
    drawCalculator();
}

void clearCalculator() {
    calc_strcpy(display, "0");
    calc_strcpy(current_number, "");
    operator_char = '\0';
    stored_number = 0;
    new_number = true;
    drawCalculator();
}
