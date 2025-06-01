#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

class Calculator {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawCalculator();
    static void processInput(char input);
    static void calculate();
    static void clearDisplay();
    static void updateDisplay();
};
void launchCalculator();
void handleCalculatorInput(uint8_t key);
void closeCalculator();
void drawCalculator();
void inputDigit(int digit);
void inputOperator(char op);
void calculateResult();
void clearCalculator();
bool isCalculatorVisible();