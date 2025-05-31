
#include "auth.hpp"
#include "../ui/window_manager.hpp"
#include "../drivers/keyboard.hpp"
#include "../debug/serial.hpp"

// Global state
static User users[MAX_USERS];
static int user_count = 0;
static bool system_locked = false;
static uint32_t system_lockout_time = 0;
static SecurityLevel system_security_level = SECURITY_PIN;
static char current_user[16] = {0};
static bool is_authenticated = false;

// Security log
#define MAX_LOG_ENTRIES 50
static char security_log[MAX_LOG_ENTRIES][80];
static int log_count = 0;

// UI state
static int auth_window_id = -1;
static char input_buffer[MAX_PASSWORD_LENGTH + 1];
static int input_pos = 0;
static bool showing_password = false;

// Utility functions
static int custom_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void custom_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static int custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

static void custom_memset(void* ptr, int value, int size) {
    char* p = (char*)ptr;
    for (int i = 0; i < size; i++) {
        p[i] = value;
    }
}

// Simple hash function (in real system, use proper cryptographic hash)
void AuthSystem::hashPassword(const char* password, char* hash) {
    uint32_t h = 5381;
    int len = custom_strlen(password);
    
    for (int i = 0; i < len; i++) {
        h = ((h << 5) + h) + password[i];
    }
    
    // Convert to string representation
    for (int i = 0; i < 8; i++) {
        hash[i] = '0' + ((h >> (i * 4)) & 0xF);
        if (hash[i] > '9') hash[i] = 'A' + (hash[i] - '9' - 1);
    }
    hash[8] = '\0';
}

bool AuthSystem::verifyPassword(const char* password, const char* stored_hash) {
    char computed_hash[9];
    hashPassword(password, computed_hash);
    return custom_strcmp(computed_hash, stored_hash) == 0;
}

uint32_t AuthSystem::getCurrentTime() {
    // Simple tick counter (in real system, use proper RTC)
    static uint32_t tick = 0;
    return ++tick;
}

User* AuthSystem::findUser(const char* username) {
    for (int i = 0; i < user_count; i++) {
        if (custom_strcmp(users[i].username, username) == 0) {
            return &users[i];
        }
    }
    return nullptr;
}

void AuthSystem::init() {
    custom_memset(users, 0, sizeof(users));
    user_count = 0;
    system_locked = false;
    is_authenticated = false;
    
    // Create default admin user
    createUser("admin", ADMIN_DEFAULT_PASSWORD, ADMIN_DEFAULT_PIN, true);
    
    logSecurityEvent("System initialized", "SYSTEM");
}

bool AuthSystem::isSystemLocked() {
    if (system_locked) {
        uint32_t current_time = getCurrentTime();
        if (current_time - system_lockout_time > LOCKOUT_TIME) {
            system_locked = false;
            logSecurityEvent("System auto-unlocked", "SYSTEM");
        }
    }
    return system_locked;
}

bool AuthSystem::isUserLocked(const char* username) {
    User* user = findUser(username);
    if (!user) return true;
    
    if (user->failed_attempts >= LOCKOUT_ATTEMPTS) {
        uint32_t current_time = getCurrentTime();
        if (current_time - user->last_lockout_time > LOCKOUT_TIME) {
            clearFailedAttempts(username);
            return false;
        }
        return true;
    }
    return false;
}

AuthResult AuthSystem::authenticateUser(const char* username, const char* credential) {
    if (isSystemLocked()) {
        return AUTH_SYSTEM_LOCKED;
    }
    
    User* user = findUser(username);
    if (!user || !user->is_active) {
        logSecurityEvent("Invalid user login attempt", username);
        return AUTH_INVALID_CREDENTIALS;
    }
    
    if (isUserLocked(username)) {
        logSecurityEvent("Locked user login attempt", username);
        return AUTH_ACCOUNT_LOCKED;
    }
    
    bool auth_success = false;
    if (system_security_level == SECURITY_PIN) {
        auth_success = custom_strcmp(user->pin, credential) == 0;
    } else {
        auth_success = verifyPassword(credential, user->password);
    }
    
    if (auth_success) {
        clearFailedAttempts(username);
        custom_strcpy(current_user, username);
        is_authenticated = true;
        logSecurityEvent("Successful login", username);
        return AUTH_SUCCESS;
    } else {
        user->failed_attempts++;
        if (user->failed_attempts >= LOCKOUT_ATTEMPTS) {
            user->last_lockout_time = getCurrentTime();
            logSecurityEvent("User locked due to failed attempts", username);
        }
        logSecurityEvent("Failed login attempt", username);
        return AUTH_INVALID_CREDENTIALS;
    }
}

AuthResult AuthSystem::authenticatePin(const char* pin) {
    if (isSystemLocked()) {
        return AUTH_SYSTEM_LOCKED;
    }
    
    // Check system PIN first
    if (custom_strcmp(pin, SYSTEM_DEFAULT_PIN) == 0) {
        custom_strcpy(current_user, "system");
        is_authenticated = true;
        logSecurityEvent("System PIN authentication", "SYSTEM");
        return AUTH_SUCCESS;
    }
    
    // Check user PINs
    for (int i = 0; i < user_count; i++) {
        if (users[i].is_active && custom_strcmp(users[i].pin, pin) == 0) {
            if (!isUserLocked(users[i].username)) {
                clearFailedAttempts(users[i].username);
                custom_strcpy(current_user, users[i].username);
                is_authenticated = true;
                logSecurityEvent("PIN authentication", users[i].username);
                return AUTH_SUCCESS;
            }
        }
    }
    
    logSecurityEvent("Invalid PIN attempt", "UNKNOWN");
    return AUTH_INVALID_CREDENTIALS;
}

bool AuthSystem::createUser(const char* username, const char* password, const char* pin, bool is_admin) {
    if (user_count >= MAX_USERS) return false;
    if (findUser(username)) return false; // User already exists
    
    User* user = &users[user_count++];
    custom_strcpy(user->username, username);
    hashPassword(password, user->password);
    custom_strcpy(user->pin, pin);
    user->security_level = SECURITY_PIN;
    user->is_admin = is_admin;
    user->is_active = true;
    user->failed_attempts = 0;
    user->last_lockout_time = 0;
    
    logSecurityEvent("User created", username);
    return true;
}

void AuthSystem::clearFailedAttempts(const char* username) {
    User* user = findUser(username);
    if (user) {
        user->failed_attempts = 0;
        user->last_lockout_time = 0;
    }
}

void AuthSystem::lockSystem() {
    system_locked = true;
    system_lockout_time = getCurrentTime();
    is_authenticated = false;
    logSecurityEvent("System locked", "SYSTEM");
}

void AuthSystem::unlockSystem() {
    system_locked = false;
    logSecurityEvent("System unlocked", "SYSTEM");
}

void AuthSystem::logSecurityEvent(const char* event, const char* username) {
    if (log_count >= MAX_LOG_ENTRIES) {
        // Shift logs
        for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) {
            custom_strcpy(security_log[i], security_log[i + 1]);
        }
        log_count = MAX_LOG_ENTRIES - 1;
    }
    
    // Format: [TIME] EVENT - USER
    char* log_entry = security_log[log_count++];
    uint32_t time = getCurrentTime();
    
    // Simple time formatting
    int pos = 0;
    log_entry[pos++] = '[';
    log_entry[pos++] = '0' + (time / 1000) % 10;
    log_entry[pos++] = '0' + (time / 100) % 10;
    log_entry[pos++] = '0' + (time / 10) % 10;
    log_entry[pos++] = '0' + time % 10;
    log_entry[pos++] = ']';
    log_entry[pos++] = ' ';
    
    // Copy event
    int event_len = custom_strlen(event);
    for (int i = 0; i < event_len && pos < 70; i++) {
        log_entry[pos++] = event[i];
    }
    
    log_entry[pos++] = ' ';
    log_entry[pos++] = '-';
    log_entry[pos++] = ' ';
    
    // Copy username
    int user_len = custom_strlen(username);
    for (int i = 0; i < user_len && pos < 79; i++) {
        log_entry[pos++] = username[i];
    }
    
    log_entry[pos] = '\0';
}

bool AuthSystem::showLoginScreen() {
    auth_window_id = WindowManager::createWindow("System Security", 15, 5, 50, 15);
    if (auth_window_id < 0) return false;
    
    WindowManager::setActiveWindow(auth_window_id);
    
    volatile char* video = (volatile char*)0xB8000;
    Window* win = WindowManager::getWindow(auth_window_id);
    if (!win) return false;
    
    // Clear window
    for (int y = win->y; y < win->y + win->height; y++) {
        for (int x = win->x; x < win->x + win->width; x++) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x1F; // White on blue
        }
    }
    
    // Draw border
    for (int x = win->x; x < win->x + win->width; x++) {
        int idx = 2 * (win->y * 80 + x);
        video[idx] = '=';
        video[idx + 1] = 0x4F;
        
        idx = 2 * ((win->y + win->height - 1) * 80 + x);
        video[idx] = '=';
        video[idx + 1] = 0x4F;
    }
    
    // Title
    const char* title = "SCos Security System";
    int title_len = custom_strlen(title);
    int title_x = win->x + (win->width - title_len) / 2;
    for (int i = 0; i < title_len; i++) {
        int idx = 2 * ((win->y + 1) * 80 + title_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x4E; // Yellow on red
    }
    
    // Security level indicator
    const char* sec_level = (system_security_level == SECURITY_PIN) ? "PIN Mode" : "Password Mode";
    int sec_x = win->x + 2;
    for (int i = 0; sec_level[i]; i++) {
        int idx = 2 * ((win->y + 3) * 80 + sec_x + i);
        video[idx] = sec_level[i];
        video[idx + 1] = 0x1A; // Green on blue
    }
    
    // Input prompt
    const char* prompt = (system_security_level == SECURITY_PIN) ? "Enter PIN:" : "Enter Password:";
    int prompt_len = custom_strlen(prompt);
    for (int i = 0; i < prompt_len; i++) {
        int idx = 2 * ((win->y + 6) * 80 + win->x + 2 + i);
        video[idx] = prompt[i];
        video[idx + 1] = 0x1F;
    }
    
    // Input field
    for (int i = 0; i < 20; i++) {
        int idx = 2 * ((win->y + 8) * 80 + win->x + 2 + i);
        video[idx] = (i < input_pos) ? '*' : '_';
        video[idx + 1] = 0x70; // Black on light gray
    }
    
    // Instructions
    const char* instructions = "Enter credentials, ESC to cancel";
    int instr_len = custom_strlen(instructions);
    for (int i = 0; i < instr_len; i++) {
        int idx = 2 * ((win->y + 11) * 80 + win->x + 2 + i);
        video[idx] = instructions[i];
        video[idx + 1] = 0x17; // Gray
    }
    
    input_pos = 0;
    custom_memset(input_buffer, 0, sizeof(input_buffer));
    
    return true;
}

bool AuthSystem::showPinScreen() {
    system_security_level = SECURITY_PIN;
    return showLoginScreen();
}

void AuthSystem::handleSecurityInput(uint8_t key) {
    if (auth_window_id < 0) return;
    
    switch (key) {
        case 0x1C: // Enter
            if (input_pos > 0) {
                input_buffer[input_pos] = '\0';
                AuthResult result;
                
                if (system_security_level == SECURITY_PIN) {
                    result = authenticatePin(input_buffer);
                } else {
                    result = authenticateUser("admin", input_buffer);
                }
                
                if (result == AUTH_SUCCESS) {
                    WindowManager::closeWindow(auth_window_id);
                    auth_window_id = -1;
                } else {
                    // Show error and reset
                    input_pos = 0;
                    custom_memset(input_buffer, 0, sizeof(input_buffer));
                    showLoginScreen(); // Refresh display
                }
            }
            break;
            
        case 0x01: // Escape
            WindowManager::closeWindow(auth_window_id);
            auth_window_id = -1;
            break;
            
        case 0x0E: // Backspace
            if (input_pos > 0) {
                input_pos--;
                input_buffer[input_pos] = '\0';
                showLoginScreen(); // Refresh display
            }
            break;
            
        default:
            // Handle alphanumeric input
            if (key >= 0x02 && key <= 0x0D) { // Numbers 1-0
                if (input_pos < MAX_PIN_LENGTH) {
                    char c = '1' + (key - 0x02);
                    if (key == 0x0B) c = '0'; // Handle 0 key
                    input_buffer[input_pos++] = c;
                    showLoginScreen(); // Refresh display
                }
            } else if (key >= 0x10 && key <= 0x19) { // Letters Q-P
                if (system_security_level == SECURITY_PASSWORD && input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'q' + (key - 0x10);
                    input_buffer[input_pos++] = c;
                    showLoginScreen(); // Refresh display
                }
            }
            break;
    }
}

bool AuthSystem::hasAdminPrivileges(const char* username) {
    User* user = findUser(username);
    return user && user->is_admin && is_authenticated;
}

SecurityLevel AuthSystem::getSystemSecurityLevel() {
    return system_security_level;
}

void AuthSystem::setSystemSecurityLevel(SecurityLevel level) {
    system_security_level = level;
    logSecurityEvent("Security level changed", current_user);
}

void AuthSystem::showSecurityLog() {
    int log_window_id = WindowManager::createWindow("Security Log", 5, 2, 70, 20);
    if (log_window_id < 0) return;
    
    WindowManager::setActiveWindow(log_window_id);
    
    volatile char* video = (volatile char*)0xB8000;
    Window* win = WindowManager::getWindow(log_window_id);
    if (!win) return;
    
    // Clear window
    for (int y = win->y; y < win->y + win->height; y++) {
        for (int x = win->x; x < win->x + win->width; x++) {
            int idx = 2 * (y * 80 + x);
            video[idx] = ' ';
            video[idx + 1] = 0x17; // Gray on black
        }
    }
    
    // Title
    const char* title = "Security Event Log";
    int title_len = custom_strlen(title);
    int title_x = win->x + (win->width - title_len) / 2;
    for (int i = 0; i < title_len; i++) {
        int idx = 2 * ((win->y + 1) * 80 + title_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x4F; // White on red
    }
    
    // Log entries
    int display_count = (log_count < 15) ? log_count : 15;
    int start_entry = (log_count > 15) ? log_count - 15 : 0;
    
    for (int i = 0; i < display_count; i++) {
        const char* entry = security_log[start_entry + i];
        int entry_len = custom_strlen(entry);
        int max_len = (entry_len < win->width - 4) ? entry_len : win->width - 4;
        
        for (int j = 0; j < max_len; j++) {
            int idx = 2 * ((win->y + 3 + i) * 80 + win->x + 2 + j);
            video[idx] = entry[j];
            video[idx + 1] = 0x1F; // White on blue
        }
    }
    
    // Footer
    const char* footer = "Press any key to close";
    int footer_len = custom_strlen(footer);
    for (int i = 0; i < footer_len; i++) {
        int idx = 2 * ((win->y + win->height - 2) * 80 + win->x + 2 + i);
        video[idx] = footer[i];
        video[idx + 1] = 0x70; // Black on gray
    }
}
#include "auth.hpp"
#include "../ui/window_manager.hpp"
#include "../drivers/keyboard.hpp"

// Security state
static bool system_locked = false;
static bool authenticated = false;
static AuthMode current_auth_mode = AUTH_PIN;
static UserProfile current_user;
static int login_window_id = -1;
static bool login_screen_active = false;
static char login_input[MAX_PASSWORD_LENGTH];
static int login_input_pos = 0;
static int failed_login_attempts = 0;
static bool input_hidden = true;

// Default credentials
static const char DEFAULT_PIN[] = "1234";
static const char DEFAULT_PASSWORD_HASH[] = "5d41402abc4b2a76b9719d911017c592"; // "hello"

// VGA functions
static void vga_put_char(int x, int y, char c, uint8_t color) {
    if (x >= 0 && x < 80 && y >= 0 && y < 25) {
        volatile char* pos = (volatile char*)0xB8000 + (y * 80 + x) * 2;
        pos[0] = c;
        pos[1] = color;
    }
}

static void vga_put_string(int x, int y, const char* str, uint8_t color) {
    for (int i = 0; str[i] && (x + i) < 80; i++) {
        vga_put_char(x + i, y, str[i], color);
    }
}

static void center_text(int y, const char* text, uint8_t color) {
    int len = 0;
    while (text[len]) len++;
    int x = (80 - len) / 2;
    vga_put_string(x, y, text, color);
}

bool SecurityManager::init() {
    // Initialize default user profile
    const char* default_username = "admin";
    for (int i = 0; i < MAX_USERNAME_LENGTH - 1 && default_username[i]; i++) {
        current_user.username[i] = default_username[i];
    }
    current_user.username[MAX_USERNAME_LENGTH - 1] = '\0';
    
    // Set default PIN
    for (int i = 0; i < MAX_PIN_LENGTH && DEFAULT_PIN[i]; i++) {
        current_user.pin[i] = DEFAULT_PIN[i];
    }
    current_user.pin[MAX_PIN_LENGTH] = '\0';
    
    // Set default password hash
    for (int i = 0; i < 32 && DEFAULT_PASSWORD_HASH[i]; i++) {
        current_user.password_hash[i] = DEFAULT_PASSWORD_HASH[i];
    }
    current_user.password_hash[32] = '\0';
    
    current_user.is_admin = true;
    current_user.failed_attempts = 0;
    current_user.locked = false;
    current_user.last_login_time = 0;
    
    system_locked = true; // System starts locked
    authenticated = false;
    login_input[0] = '\0';
    login_input_pos = 0;
    
    return true;
}

bool SecurityManager::authenticate(const char* input, AuthMode mode) {
    if (current_user.locked) {
        return false;
    }
    
    bool result = false;
    
    switch (mode) {
        case AUTH_PIN: {
            int i = 0;
            while (i < MAX_PIN_LENGTH && input[i] && current_user.pin[i]) {
                if (input[i] != current_user.pin[i]) {
                    break;
                }
                i++;
            }
            result = (input[i] == '\0' && current_user.pin[i] == '\0');
            break;
        }
        case AUTH_PASSWORD:
            result = verifyHash(input, current_user.password_hash);
            break;
        case AUTH_BOTH:
            // For now, just check PIN (could be extended)
            result = authenticate(input, AUTH_PIN);
            break;
        default:
            result = true; // No auth
            break;
    }
    
    if (result) {
        authenticated = true;
        system_locked = false;
        failed_login_attempts = 0;
        current_user.failed_attempts = 0;
    } else {
        failed_login_attempts++;
        current_user.failed_attempts++;
        
        if (failed_login_attempts >= 3) {
            current_user.locked = true;
        }
    }
    
    return result;
}

void SecurityManager::lockSystem() {
    system_locked = true;
    authenticated = false;
}

void SecurityManager::unlockSystem() {
    if (authenticated) {
        system_locked = false;
    }
}

bool SecurityManager::isSystemLocked() {
    return system_locked;
}

bool SecurityManager::isAuthenticated() {
    return authenticated;
}

void SecurityManager::logout() {
    authenticated = false;
    system_locked = true;
    clearLoginInput();
}

void SecurityManager::showLoginScreen() {
    login_window_id = WindowManager::createWindow("System Security", 20, 6, 40, 14);
    if (login_window_id >= 0) {
        login_screen_active = true;
        WindowManager::setActiveWindow(login_window_id);
        drawLoginScreen();
    }
}

void SecurityManager::drawLoginScreen() {
    if (!login_screen_active || login_window_id < 0) return;
    
    Window* win = WindowManager::getWindow(login_window_id);
    if (!win) return;
    
    volatile char* video = (volatile char*)0xB8000;
    
    // Clear window area
    for (int y = win->y + 1; y < win->y + win->height - 1; y++) {
        for (int x = win->x + 1; x < win->x + win->width - 1; x++) {
            vga_put_char(x, y, ' ', 0x07);
        }
    }
    
    int start_x = win->x + 2;
    int start_y = win->y + 2;
    
    // Title
    vga_put_string(start_x + 8, start_y, "SCos Security Login", 0x4F);
    
    // Security status
    if (current_user.locked) {
        vga_put_string(start_x + 5, start_y + 2, "ACCOUNT LOCKED", 0x4E);
        vga_put_string(start_x + 2, start_y + 3, "Contact administrator", 0x07);
        return;
    }
    
    // Username display
    vga_put_string(start_x, start_y + 3, "User: ", 0x07);
    vga_put_string(start_x + 6, start_y + 3, current_user.username, 0x0F);
    
    // Auth mode display
    const char* mode_text = "PIN";
    if (current_auth_mode == AUTH_PASSWORD) mode_text = "Password";
    else if (current_auth_mode == AUTH_BOTH) mode_text = "PIN + Password";
    
    vga_put_string(start_x, start_y + 5, "Mode: ", 0x07);
    vga_put_string(start_x + 6, start_y + 5, mode_text, 0x0E);
    
    // Input prompt
    if (current_auth_mode == AUTH_PIN) {
        vga_put_string(start_x, start_y + 7, "Enter PIN: ", 0x07);
    } else {
        vga_put_string(start_x, start_y + 7, "Enter Password: ", 0x07);
    }
    
    // Input field (hidden)
    int input_x = start_x + (current_auth_mode == AUTH_PIN ? 11 : 16);
    for (int i = 0; i < login_input_pos; i++) {
        vga_put_char(input_x + i, start_y + 7, '*', 0x0F);
    }
    
    // Cursor
    vga_put_char(input_x + login_input_pos, start_y + 7, '_', 0x0F);
    
    // Failed attempts warning
    if (failed_login_attempts > 0) {
        char attempts_msg[32];
        attempts_msg[0] = 'F'; attempts_msg[1] = 'a'; attempts_msg[2] = 'i'; attempts_msg[3] = 'l';
        attempts_msg[4] = 'e'; attempts_msg[5] = 'd'; attempts_msg[6] = ' '; attempts_msg[7] = 'a';
        attempts_msg[8] = 't'; attempts_msg[9] = 't'; attempts_msg[10] = 'e'; attempts_msg[11] = 'm';
        attempts_msg[12] = 'p'; attempts_msg[13] = 't'; attempts_msg[14] = 's'; attempts_msg[15] = ':';
        attempts_msg[16] = ' '; attempts_msg[17] = '0' + failed_login_attempts; attempts_msg[18] = '/';
        attempts_msg[19] = '3'; attempts_msg[20] = '\0';
        
        vga_put_string(start_x, start_y + 9, attempts_msg, 0x4E);
    }
    
    // Instructions
    vga_put_string(start_x, start_y + 11, "Enter: Login | Esc: Cancel", 0x08);
}

void SecurityManager::handleLoginInput(uint8_t key) {
    if (!login_screen_active) return;
    
    switch (key) {
        case 0x01: // Escape
            if (login_window_id >= 0) {
                WindowManager::closeWindow(login_window_id);
                login_screen_active = false;
                login_window_id = -1;
                clearLoginInput();
            }
            break;
            
        case 0x0E: // Backspace
            if (login_input_pos > 0) {
                login_input_pos--;
                login_input[login_input_pos] = '\0';
                drawLoginScreen();
            }
            break;
            
        case 0x1C: // Enter
            login_input[login_input_pos] = '\0';
            if (authenticate(login_input, current_auth_mode)) {
                // Success
                if (login_window_id >= 0) {
                    WindowManager::closeWindow(login_window_id);
                    login_screen_active = false;
                    login_window_id = -1;
                }
                clearLoginInput();
            } else {
                // Failed
                clearLoginInput();
                drawLoginScreen();
            }
            break;
            
        default:
            // Regular character input
            if (key >= 0x02 && key <= 0x0D) { // Number keys 1-0
                char digit = '1' + (key - 0x02);
                if (key == 0x0B) digit = '0'; // Special case for 0
                
                if (login_input_pos < (current_auth_mode == AUTH_PIN ? MAX_PIN_LENGTH : MAX_PASSWORD_LENGTH) - 1) {
                    login_input[login_input_pos] = digit;
                    login_input_pos++;
                    login_input[login_input_pos] = '\0';
                    drawLoginScreen();
                }
            }
            // Add letter support for passwords
            else if (current_auth_mode != AUTH_PIN && key >= 0x10 && key <= 0x32) {
                char letter = 'a';
                // Simple scancode to letter mapping
                if (key >= 0x10 && key <= 0x19) letter = 'q' + (key - 0x10);
                else if (key >= 0x1E && key <= 0x26) letter = 'a' + (key - 0x1E);
                else if (key >= 0x2C && key <= 0x32) letter = 'z' + (key - 0x2C);
                
                if (login_input_pos < MAX_PASSWORD_LENGTH - 1) {
                    login_input[login_input_pos] = letter;
                    login_input_pos++;
                    login_input[login_input_pos] = '\0';
                    drawLoginScreen();
                }
            }
            break;
    }
}

void SecurityManager::clearLoginInput() {
    login_input[0] = '\0';
    login_input_pos = 0;
}

bool SecurityManager::changePin(const char* old_pin, const char* new_pin) {
    if (!authenticated) return false;
    
    // Verify old PIN
    if (!authenticate(old_pin, AUTH_PIN)) {
        return false;
    }
    
    // Set new PIN
    int i;
    for (i = 0; i < MAX_PIN_LENGTH && new_pin[i]; i++) {
        current_user.pin[i] = new_pin[i];
    }
    current_user.pin[i] = '\0';
    
    return true;
}

bool SecurityManager::changePassword(const char* old_password, const char* new_password) {
    if (!authenticated) return false;
    
    // Verify old password
    if (!verifyHash(old_password, current_user.password_hash)) {
        return false;
    }
    
    // Hash new password (simple hash)
    uint32_t hash = simpleHash(new_password);
    
    // Convert hash to string (simplified)
    for (int i = 0; i < 32; i++) {
        current_user.password_hash[i] = '0' + ((hash >> (i % 32)) & 1);
    }
    current_user.password_hash[32] = '\0';
    
    return true;
}

uint32_t SecurityManager::simpleHash(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

bool SecurityManager::verifyHash(const char* input, const char* stored_hash) {
    uint32_t input_hash = simpleHash(input);
    uint32_t stored = 0;
    
    // Simple hash comparison (in real system, use proper crypto)
    for (int i = 0; i < 32; i++) {
        if (stored_hash[i] >= '0' && stored_hash[i] <= '9') {
            stored = (stored << 1) | (stored_hash[i] - '0');
        }
    }
    
    return input_hash == stored;
}

void SecurityManager::resetFailedAttempts() {
    failed_login_attempts = 0;
    current_user.failed_attempts = 0;
    current_user.locked = false;
}

int SecurityManager::getFailedAttempts() {
    return failed_login_attempts;
}

void SecurityManager::setAuthMode(AuthMode mode) {
    current_auth_mode = mode;
}

AuthMode SecurityManager::getAuthMode() {
    return current_auth_mode;
}
