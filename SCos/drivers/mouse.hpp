#ifndef MOUSE_HPP
#define MOUSE_HPP

#include <stdint.h>

#define MOUSE_LEFT_BUTTON 0x01
#define MOUSE_RIGHT_BUTTON 0x02
#define MOUSE_MIDDLE_BUTTON 0x04

#define MATRIX_GREEN 1
#define MATRIX_RED 2
#define MATRIX_PURPLE 3

#ifndef THEME
#define THEME MATRIX_GREEN
#endif

#define BACKGROUND_COLOR 0x000000
#define TEXT_COLOR 0x39FF14
#define SECONDARY_COLOR 0x39FF14

struct MouseState {
    int x, y;
    uint8_t buttons;
    int8_t delta_x, delta_y;
    int8_t scroll_wheel;
};

class Mouse {
private:
    static MouseState current_state;
    static MouseState previous_state;
    static bool initialized;

public:
    static bool init();

    static MouseState getState();
    static int getX() { return current_state.x; }
    static int getY() { return current_state.y; }
    static bool isLeftButtonPressed() { return current_state.buttons & MOUSE_LEFT_BUTTON; }
    static bool isRightButtonPressed() { return current_state.buttons & MOUSE_RIGHT_BUTTON; }
    static bool isMiddleButtonPressed() { return current_state.buttons & MOUSE_MIDDLE_BUTTON; }

    static bool wasLeftButtonClicked();
    static bool wasRightButtonClicked();
    static bool wasMiddleButtonClicked();

    static void update();
    static void handleMousePacket(uint8_t packet1, uint8_t packet2, uint8_t packet3);

    static void drawCursor();
    static void hideCursor();

    static void clampPosition();

    static void sendCommand(uint8_t command);
    static uint8_t readData();
    static void enableScrollWheel();

    static void setPosition(int x, int y);

};

extern "C" void mouse_handler();

#endif // MOUSE_HPP
