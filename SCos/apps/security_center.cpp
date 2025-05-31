
#include "../security/auth.hpp"
#include "../ui/window_manager.hpp"

// Forward declaration for SecurityManager if not in auth.hpp
class SecurityManager {
public:
    static bool isAuthenticated();
    static AuthMode getAuthMode();
    static void setAuthMode(AuthMode mode);
    static int getFailedAttempts();
    static bool changePin(const char* old_pin, const char* new_pin);
    static void lockSystem();
    static void showLoginScreen();
    static void resetFailedAttempts();
};

// Security center state
static int security_window_id = -1;
static int selected_option = 0;
static int current_menu = 0; // 0=main, 1=change_pin, 2=change_password, 3=settings
static char input_buffer[64];
static int input_pos = 0;
static bool input_mode = false;

// VGA functions (remove static to match header declarations)
void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < 80 && y >= 0 && y < 25) {
        volatile char* pos = (volatile char*)0xB8000 + (y * 80 + x) * 2;
        pos[0] = c;
        pos[1] = color;
    }
}

void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < 80; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

static void draw_security_main_menu() {
    Window* win = WindowManager::getWindow(security_window_id);
    if (!win) return;
    
    int start_x = win->x + 2;
    int start_y = win->y + 2;
    
    // Clear window content area
    for (int y = win->y + 1; y < win->y + win->height - 1; y++) {
        for (int x = win->x + 1; x < win->x + win->width - 1; x++) {
            vga_put_char(x, y, ' ', 0x07);
        }
    }
    
    // Title
    vga_put_string(start_x + 12, start_y, "Security Center", 0x4F);
    
    // Security status
    const char* status = SecurityManager::isAuthenticated() ? "UNLOCKED" : "LOCKED";
    uint8_t status_color = SecurityManager::isAuthenticated() ? 0x2F : 0x4F;
    vga_put_string(start_x, start_y + 2, "System Status: ", 0x07);
    vga_put_string(start_x + 15, start_y + 2, status, status_color);
    
    // Menu options
    const char* menu_items[] = {
        "Change PIN",
        "Change Password", 
        "Authentication Mode",
        "Lock System",
        "Reset Failed Attempts",
        "Security Settings"
    };
    
    int num_items = 6;
    
    vga_put_string(start_x, start_y + 4, "Security Options:", 0x0E);
    
    for (int i = 0; i < num_items; i++) {
        uint8_t color = (i == selected_option) ? 0x70 : 0x07;
        char prefix = (i == selected_option) ? '>' : ' ';
        
        vga_put_char(start_x + 2, start_y + 6 + i, prefix, color);
        vga_put_string(start_x + 4, start_y + 6 + i, menu_items[i], color);
    }
    
    // Current auth mode
    const char* mode_names[] = {"None", "PIN", "Password", "PIN + Password"};
    AuthMode mode = SecurityManager::getAuthMode();
    vga_put_string(start_x, start_y + 13, "Auth Mode: ", 0x08);
    vga_put_string(start_x + 11, start_y + 13, mode_names[mode], 0x0F);
    
    // Failed attempts
    int attempts = SecurityManager::getFailedAttempts();
    char attempts_str[32] = "Failed Attempts: ";
    attempts_str[17] = '0' + attempts;
    attempts_str[18] = '\0';
    vga_put_string(start_x, start_y + 14, attempts_str, attempts > 0 ? 0x4F : 0x08);
}

static void draw_pin_change_screen() {
    Window* win = WindowManager::getWindow(security_window_id);
    if (!win) return;
    
    int start_x = win->x + 2;
    int start_y = win->y + 2;
    
    // Clear window content area
    for (int y = win->y + 1; y < win->y + win->height - 1; y++) {
        for (int x = win->x + 1; x < win->x + win->width - 1; x++) {
            vga_put_char(x, y, ' ', 0x07);
        }
    }
    
    vga_put_string(start_x + 15, start_y, "Change PIN", 0x4F);
    
    if (input_pos == 0) {
        vga_put_string(start_x, start_y + 3, "Enter current PIN:", 0x07);
    } else if (input_pos <= 8) {
        vga_put_string(start_x, start_y + 3, "Enter new PIN (4-8 digits):", 0x07);
    }
    
    // Show masked input
    vga_put_string(start_x, start_y + 5, "PIN: ", 0x07);
    for (int i = 0; i < (input_pos > 8 ? input_pos - 8 : input_pos); i++) {
        vga_put_char(start_x + 5 + i, start_y + 5, '*', 0x0F);
    }
    vga_put_char(start_x + 5 + (input_pos > 8 ? input_pos - 8 : input_pos), start_y + 5, '_', 0x0F);
    
    vga_put_string(start_x, start_y + 12, "Enter: Confirm | Esc: Cancel", 0x08);
}

void openSecurityCenter() {
    security_window_id = WindowManager::createWindow("Security Center", 15, 4, 50, 16);
    if (security_window_id >= 0) {
        WindowManager::setActiveWindow(security_window_id);
        current_menu = 0;
        selected_option = 0;
        input_mode = false;
        input_pos = 0;
        draw_security_main_menu();
    }
}

static void handleSecurityInput(uint8_t key) {
    if (security_window_id < 0) return;
    
    if (input_mode) {
        switch (key) {
            case 0x01: // Escape
                input_mode = false;
                current_menu = 0;
                input_pos = 0;
                draw_security_main_menu();
                break;
                
            case 0x0E: // Backspace
                if (input_pos > 0) {
                    input_pos--;
                    input_buffer[input_pos] = '\0';
                    if (current_menu == 1) draw_pin_change_screen();
                }
                break;
                
            case 0x1C: // Enter
                input_buffer[input_pos] = '\0';
                if (current_menu == 1) { // PIN change
                    if (input_pos <= 8) {
                        // First PIN entered, now get new PIN
                        input_pos = 9; // Mark transition
                        draw_pin_change_screen();
                    } else {
                        // Both PINs entered
                        char old_pin[9], new_pin[9];
                        int i;
                        for (i = 0; i < 8 && input_buffer[i]; i++) {
                            old_pin[i] = input_buffer[i];
                        }
                        old_pin[i] = '\0';
                        
                        for (i = 8; i < input_pos && input_buffer[i]; i++) {
                            new_pin[i-8] = input_buffer[i];
                        }
                        new_pin[i-8] = '\0';
                        
                        if (SecurityManager::changePin(old_pin, new_pin)) {
                            // Success - return to main menu
                            input_mode = false;
                            current_menu = 0;
                            input_pos = 0;
                            draw_security_main_menu();
                        } else {
                            // Failed - reset
                            input_pos = 0;
                            draw_pin_change_screen();
                        }
                    }
                }
                break;
                
            default:
                // Number input for PIN
                if (key >= 0x02 && key <= 0x0B && input_pos < 63) {
                    char digit = (key == 0x0B) ? '0' : ('1' + key - 0x02);
                    input_buffer[input_pos] = digit;
                    input_pos++;
                    if (current_menu == 1) draw_pin_change_screen();
                }
                break;
        }
    } else {
        switch (key) {
            case 0x01: // Escape
                if (security_window_id >= 0) {
                    WindowManager::closeWindow(security_window_id);
                    security_window_id = -1;
                }
                break;
                
            case 0x48: // Up arrow
                if (selected_option > 0) {
                    selected_option--;
                    draw_security_main_menu();
                }
                break;
                
            case 0x50: // Down arrow
                if (selected_option < 5) {
                    selected_option++;
                    draw_security_main_menu();
                }
                break;
                
            case 0x1C: // Enter
                switch (selected_option) {
                    case 0: // Change PIN
                        current_menu = 1;
                        input_mode = true;
                        input_pos = 0;
                        draw_pin_change_screen();
                        break;
                    case 1: // Change Password
                        // TODO: Implement password change
                        break;
                    case 2: // Authentication Mode
                        {
                            AuthMode current = SecurityManager::getAuthMode();
                            AuthMode next = (AuthMode)((current + 1) % 4);
                            SecurityManager::setAuthMode(next);
                            draw_security_main_menu();
                        }
                        break;
                    case 3: // Lock System
                        SecurityManager::lockSystem();
                        SecurityManager::showLoginScreen();
                        if (security_window_id >= 0) {
                            WindowManager::closeWindow(security_window_id);
                            security_window_id = -1;
                        }
                        break;
                    case 4: // Reset Failed Attempts
                        SecurityManager::resetFailedAttempts();
                        draw_security_main_menu();
                        break;
                    case 5: // Security Settings
                        // TODO: Implement advanced settings
                        break;
                }
                break;
        }
    }
}

// Security center class implementation
void SecurityCenter::handleInput(uint8_t key) {
    handleSecurityInput(key);
}
