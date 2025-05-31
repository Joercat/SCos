
#include "mouse.hpp"
#include "../ui/window_manager.hpp"

// PS/2 mouse ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 mouse commands
#define MOUSE_ENABLE_PACKET_STREAMING 0xF4
#define MOUSE_DISABLE_PACKET_STREAMING 0xF5
#define MOUSE_SET_SAMPLE_RATE 0xF3
#define MOUSE_GET_MOUSE_ID 0xF2
#define MOUSE_SET_RESOLUTION 0xE8
#define MOUSE_RESET 0xFF

// Static member definitions
MouseState Mouse::current_state = {40, 12, 0, 0, 0, 0};
MouseState Mouse::previous_state = {40, 12, 0, 0, 0, 0};
bool Mouse::initialized = false;

// Port I/O functions (simplified for freestanding environment)
static inline void outb(uint16_t port, uint8_t data) {
    asm volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    asm volatile ("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static void wait_for_input() {
    while ((inb(PS2_STATUS_PORT) & 0x02) != 0);
}

static void wait_for_output() {
    while ((inb(PS2_STATUS_PORT) & 0x01) == 0);
}

bool Mouse::init() {
    if (initialized) return true;
    
    // Enable auxiliary mouse device
    wait_for_input();
    outb(PS2_COMMAND_PORT, 0xA8);
    
    // Enable mouse interrupts
    wait_for_input();
    outb(PS2_COMMAND_PORT, 0x20);
    wait_for_output();
    uint8_t status = inb(PS2_DATA_PORT) | 2;
    wait_for_input();
    outb(PS2_COMMAND_PORT, 0x60);
    wait_for_input();
    outb(PS2_DATA_PORT, status);
    
    // Set mouse defaults
    sendCommand(MOUSE_RESET);
    sendCommand(MOUSE_SET_SAMPLE_RATE);
    sendCommand(100); // Sample rate
    sendCommand(MOUSE_SET_RESOLUTION);
    sendCommand(3); // Resolution
    
    // Enable scroll wheel
    enableScrollWheel();
    
    // Enable packet streaming
    sendCommand(MOUSE_ENABLE_PACKET_STREAMING);
    
    initialized = true;
    return true;
}

void Mouse::sendCommand(uint8_t command) {
    wait_for_input();
    outb(PS2_COMMAND_PORT, 0xD4);
    wait_for_input();
    outb(PS2_DATA_PORT, command);
}

uint8_t Mouse::readData() {
    wait_for_output();
    return inb(PS2_DATA_PORT);
}

void Mouse::enableScrollWheel() {
    // Magic sequence to enable scroll wheel
    sendCommand(MOUSE_SET_SAMPLE_RATE);
    sendCommand(200);
    sendCommand(MOUSE_SET_SAMPLE_RATE);
    sendCommand(100);
    sendCommand(MOUSE_SET_SAMPLE_RATE);
    sendCommand(80);
    sendCommand(MOUSE_GET_MOUSE_ID);
}

MouseState Mouse::getState() {
    return current_state;
}

bool Mouse::wasLeftButtonClicked() {
    return (current_state.buttons & MOUSE_LEFT_BUTTON) && 
           !(previous_state.buttons & MOUSE_LEFT_BUTTON);
}

bool Mouse::wasRightButtonClicked() {
    return (current_state.buttons & MOUSE_RIGHT_BUTTON) && 
           !(previous_state.buttons & MOUSE_RIGHT_BUTTON);
}

bool Mouse::wasMiddleButtonClicked() {
    return (current_state.buttons & MOUSE_MIDDLE_BUTTON) && 
           !(previous_state.buttons & MOUSE_MIDDLE_BUTTON);
}

void Mouse::handleMousePacket(uint8_t packet1, uint8_t packet2, uint8_t packet3) {
    previous_state = current_state;
    
    // Parse mouse packet
    current_state.buttons = packet1 & 0x07;
    
    // Handle movement (convert to signed)
    current_state.delta_x = packet2;
    current_state.delta_y = packet3;
    
    if (packet1 & 0x10) current_state.delta_x |= 0xFFFFFF00; // Sign extend
    if (packet1 & 0x20) current_state.delta_y |= 0xFFFFFF00; // Sign extend
    
    // Update position
    current_state.x += current_state.delta_x;
    current_state.y -= current_state.delta_y; // Y is inverted
    
    clampPosition();
    drawCursor();
}

void Mouse::clampPosition() {
    if (current_state.x < 0) current_state.x = 0;
    if (current_state.x >= 80) current_state.x = 79;
    if (current_state.y < 0) current_state.y = 0;
    if (current_state.y >= 25) current_state.y = 24;
}

void Mouse::drawCursor() {
    volatile char* video = (volatile char*)0xB8000;
    
    // Clear previous cursor position
    if (previous_state.x != current_state.x || previous_state.y != current_state.y) {
        int prev_idx = 2 * (previous_state.y * 80 + previous_state.x);
        // Restore original character (simplified - should store/restore properly)
        video[prev_idx + 1] &= 0x0F; // Remove cursor highlight
    }
    
    // Draw new cursor
    int idx = 2 * (current_state.y * 80 + current_state.x);
    char cursor_char = (current_state.buttons & MOUSE_LEFT_BUTTON) ? 'X' : '*';
    uint8_t cursor_color = (current_state.buttons & MOUSE_LEFT_BUTTON) ? 0x4F : 0x1F;
    
    video[idx] = cursor_char;
    video[idx + 1] = cursor_color;
}

void Mouse::hideCursor() {
    volatile char* video = (volatile char*)0xB8000;
    int idx = 2 * (current_state.y * 80 + current_state.x);
    video[idx + 1] &= 0x0F; // Remove cursor highlight
}

void Mouse::update() {
    drawCursor();
}

// Mouse interrupt handler (to be called from IRQ12)
extern "C" void mouse_handler() {
    static uint8_t mouse_cycle = 0;
    static uint8_t mouse_packet[3];
    
    uint8_t data = inb(PS2_DATA_PORT);
    
    switch (mouse_cycle) {
        case 0:
            mouse_packet[0] = data;
            if (data & 0x08) mouse_cycle++; // Valid packet start
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_packet[2] = data;
            Mouse::handleMousePacket(mouse_packet[0], mouse_packet[1], mouse_packet[2]);
            mouse_cycle = 0;
            break;
    }
}
