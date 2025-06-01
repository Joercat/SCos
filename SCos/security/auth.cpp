#include "auth.hpp"
#include "../ui/window_manager.hpp"
#include "../drivers/keyboard.hpp"
#include "../debug/serial.hpp"

// Additional types needed for the implementation
#define MAX_USERS 10
#define LOCKOUT_ATTEMPTS 3
#define LOCKOUT_TIME 300
#define ADMIN_DEFAULT_PASSWORD "admin123"
#define ADMIN_DEFAULT_PIN "1234"
#define SYSTEM_DEFAULT_PIN "0000"

enum SecurityLevel {
    SECURITY_NONE = 0,
    SECURITY_PIN = 1,
    SECURITY_PASSWORD = 2
};

// AuthResult enum definitions
#define AUTH_SUCCESS 0
#define AUTH_INVALID_CREDENTIALS 1
#define AUTH_ACCOUNT_LOCKED 2
#define AUTH_SYSTEM_LOCKED 3
#define AUTH_FAILED 4
#define AUTH_LOCKED 5
#define AUTH_TIMEOUT 6

struct User {
    char username[MAX_USERNAME_LENGTH];
    char password[33]; // Hash
    char pin[MAX_PIN_LENGTH + 1];
    SecurityLevel security_level;
    bool is_admin;
    bool is_active;
    int failed_attempts;
    uint32_t last_lockout_time;
};

// Global state
static User users[MAX_USERS];
static int user_count = 0;
static bool system_locked = true;
static uint32_t system_lockout_time = 0;
static SecurityLevel system_security_level = SECURITY_PIN;
static char current_user[16] = "Administrator";
static bool is_authenticated = false;

// Security log
#define MAX_LOG_ENTRIES 50
static char security_log[MAX_LOG_ENTRIES][80];
static int log_count = 0;

// Lock screen UI state
static bool lock_screen_visible = false;
static char login_input[MAX_PASSWORD_LENGTH + 1];
static int login_input_pos = 0;
static bool show_password_field = false;
static bool caps_lock_on = false;
static char user_avatar = 'A';

// SecurityManager implementation
static AuthMode current_auth_mode = AUTH_PIN;
static char stored_pin[MAX_PIN_LENGTH + 1] = "1234";
static char stored_password_hash[33] = "";
static int failed_login_attempts = 0;
static bool security_authenticated = false;

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

// Background pattern for lock screen
static void drawBackgroundPattern() {
    volatile char* video = (volatile char*)0xB8000;

    for (int y = 0; y < 25; y++) {
        for (int x = 0; x < 80; x++) {
            int idx = 2 * (y * 80 + x);
            char pattern_char = ' ';
            uint8_t bg_color = 0x10; // Dark blue base

            if ((x + y) % 4 == 0) {
                bg_color = 0x11;
            } else if ((x + y) % 6 == 0) {
                pattern_char = '.';
                bg_color = 0x19;
            } else if ((x * y) % 23 == 0) {
                bg_color = 0x13;
            }

            video[idx] = pattern_char;
            video[idx + 1] = bg_color;
        }
    }
}

static void drawUserAvatar(int center_x, int avatar_y) {
    volatile char* video = (volatile char*)0xB8000;

    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            int x = center_x + dx;
            int y = avatar_y + dy;
            if (x >= 0 && x < 80 && y >= 0 && y < 25) {
                int idx = 2 * (y * 80 + x);
                if (dx == 0 && dy == 0) {
                    video[idx] = user_avatar;
                    video[idx + 1] = 0x4F;
                } else if ((dx == -3 || dx == 3) || (dy == -2 || dy == 2)) {
                    video[idx] = '#';
                    video[idx + 1] = 0x77;
                } else {
                    video[idx] = ' ';
                    video[idx + 1] = 0x77;
                }
            }
        }
    }
}

static void drawLockScreen() {
    drawBackgroundPattern();

    volatile char* video = (volatile char*)0xB8000;
    int center_x = 40;

    // Draw SCos logo/title at top
    const char* title = "SCos";
    int title_x = center_x - 2;
    for (int i = 0; title[i]; i++) {
        int idx = 2 * (3 * 80 + title_x + i);
        video[idx] = title[i];
        video[idx + 1] = 0x1F;
    }

    // Draw user avatar
    int avatar_y = 8;
    drawUserAvatar(center_x, avatar_y);

    // Draw username below avatar
    int username_y = avatar_y + 4;
    int username_len = custom_strlen(current_user);
    int username_x = center_x - (username_len / 2);
    for (int i = 0; current_user[i]; i++) {
        int idx = 2 * (username_y * 80 + username_x + i);
        video[idx] = current_user[i];
        video[idx + 1] = 0x1F;
    }

    // Draw password/PIN input field
    int input_y = username_y + 3;
    int input_width = 24;
    int input_x = center_x - (input_width / 2);

    // Input field background
    for (int i = 0; i < input_width; i++) {
        int idx = 2 * (input_y * 80 + input_x + i);
        video[idx] = ' ';
        video[idx + 1] = 0x70;
    }

    // Input field border
    for (int i = -1; i <= input_width; i++) {
        if (input_x + i >= 0 && input_x + i < 80) {
            int idx = 2 * ((input_y - 1) * 80 + input_x + i);
            video[idx] = (i == -1 || i == input_width) ? '+' : '-';
            video[idx + 1] = 0x1F;

            idx = 2 * ((input_y + 1) * 80 + input_x + i);
            video[idx] = (i == -1 || i == input_width) ? '+' : '-';
            video[idx + 1] = 0x1F;
        }
    }

    // Side borders
    int idx = 2 * (input_y * 80 + input_x - 1);
    video[idx] = '|';
    video[idx + 1] = 0x1F;

    idx = 2 * (input_y * 80 + input_x + input_width);
    video[idx] = '|';
    video[idx + 1] = 0x1F;

    // Show input (as dots for security)
    for (int i = 0; i < login_input_pos && i < input_width - 2; i++) {
        int idx = 2 * (input_y * 80 + input_x + 1 + i);
        video[idx] = (current_auth_mode == AUTH_PIN) ? '*' : '*';
        video[idx + 1] = 0x70;
    }

    // Cursor
    if (login_input_pos < input_width - 2) {
        int idx = 2 * (input_y * 80 + input_x + 1 + login_input_pos);
        video[idx] = '_';
        video[idx + 1] = 0x70;
    }

    // Instructions
    const char* instruction = (current_auth_mode == AUTH_PIN) ? 
                              "Enter your PIN" : "Enter your password";
    int instr_len = custom_strlen(instruction);
    int instr_x = center_x - (instr_len / 2);
    int instr_y = input_y + 3;
    for (int i = 0; instruction[i]; i++) {
        int idx = 2 * (instr_y * 80 + instr_x + i);
        video[idx] = instruction[i];
        video[idx + 1] = 0x1E;
    }

    // Show caps lock status
    if (caps_lock_on) {
        const char* caps_msg = "Caps Lock is on";
        int caps_len = custom_strlen(caps_msg);
        int caps_x = center_x - (caps_len / 2);
        int caps_y = instr_y + 2;
        for (int i = 0; caps_msg[i]; i++) {
            int idx = 2 * (caps_y * 80 + caps_x + i);
            video[idx] = caps_msg[i];
            video[idx + 1] = 0x4E;
        }
    }

    // Bottom instructions
    const char* bottom_help = "Press Enter to sign in | ESC to switch user";
    int help_len = custom_strlen(bottom_help);
    int help_x = center_x - (help_len / 2);
    int help_y = 22;
    for (int i = 0; bottom_help[i]; i++) {
        int idx = 2 * (help_y * 80 + help_x + i);
        video[idx] = bottom_help[i];
        video[idx + 1] = 0x17;
    }

    // Date and time (simplified)
    const char* datetime = "Monday, December 15  12:45 PM";
    int dt_len = custom_strlen(datetime);
    int dt_x = 80 - dt_len - 2;
    int dt_y = 1;
    for (int i = 0; datetime[i]; i++) {
        int idx = 2 * (dt_y * 80 + dt_x + i);
        video[idx] = datetime[i];
        video[idx + 1] = 0x1F;
    }
}

// Simple hash function
static uint32_t simpleHash(const char* str) {
    uint32_t hash = 5381;
    int len = custom_strlen(str);
    for (int i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + str[i];
    }
    return hash;
}

static bool verifyHash(const char* input, const char* stored_hash) {
    uint32_t input_hash = simpleHash(input);
    uint32_t stored = 0;

    // Convert stored hash string back to number for comparison
    for (int i = 0; stored_hash[i]; i++) {
        stored = stored * 10 + (stored_hash[i] - '0');
    }

    return input_hash == stored;
}

// SecurityManager implementation
bool SecurityManager::init() {
    custom_memset(users, 0, sizeof(users));
    user_count = 0;
    system_locked = true;
    is_authenticated = false;
    lock_screen_visible = true;
    security_authenticated = false;
    failed_login_attempts = 0;
    current_auth_mode = AUTH_PIN;

    // Initialize stored password hash
    uint32_t hash = simpleHash("admin123");
    for (int i = 7; i >= 0; i--) {
        stored_password_hash[i] = '0' + (hash % 10);
        hash /= 10;
    }
    stored_password_hash[8] = '\0';

    return true;
}

bool SecurityManager::authenticate(const char* input, AuthMode mode) {
    if (mode == AUTH_PIN) {
        if (custom_strcmp(input, stored_pin) == 0) {
            security_authenticated = true;
            failed_login_attempts = 0;
            return true;
        }
    } else if (mode == AUTH_PASSWORD) {
        if (verifyHash(input, stored_password_hash)) {
            security_authenticated = true;
            failed_login_attempts = 0;
            return true;
        }
    }

    failed_login_attempts++;
    return false;
}

void SecurityManager::lockSystem() {
    system_locked = true;
    lock_screen_visible = true;
    security_authenticated = false;
    drawLockScreen();
}

void SecurityManager::unlockSystem() {
    system_locked = false;
    lock_screen_visible = false;

    // Clear screen for desktop
    volatile char* video = (volatile char*)0xB8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video[i] = ' ';
        video[i + 1] = 0x07;
    }
}

bool SecurityManager::isSystemLocked() {
    return system_locked;
}

bool SecurityManager::changePin(const char* old_pin, const char* new_pin) {
    if (custom_strcmp(old_pin, stored_pin) == 0) {
        custom_strcpy(stored_pin, new_pin);
        return true;
    }
    return false;
}

bool SecurityManager::changePassword(const char* old_password, const char* new_password) {
    if (verifyHash(old_password, stored_password_hash)) {
        uint32_t hash = simpleHash(new_password);
        for (int i = 7; i >= 0; i--) {
            stored_password_hash[i] = '0' + (hash % 10);
            hash /= 10;
        }
        stored_password_hash[8] = '\0';
        return true;
    }
    return false;
}

void SecurityManager::showLoginScreen() {
    lock_screen_visible = true;
    login_input_pos = 0;
    custom_memset(login_input, 0, sizeof(login_input));
    drawLockScreen();
}

void SecurityManager::handleLoginInput(uint8_t key) {
    if (!lock_screen_visible) return;

    switch (key) {
        case 0x1C: // Enter
            if (login_input_pos > 0) {
                login_input[login_input_pos] = '\0';

                if (authenticate(login_input, current_auth_mode)) {
                    unlockSystem();
                } else {
                    // Show error and reset
                    login_input_pos = 0;
                    custom_memset(login_input, 0, sizeof(login_input));
                    drawLockScreen();
                }
            }
            break;

        case 0x01: // Escape - Switch auth mode
            current_auth_mode = (current_auth_mode == AUTH_PIN) ? AUTH_PASSWORD : AUTH_PIN;
            login_input_pos = 0;
            custom_memset(login_input, 0, sizeof(login_input));
            drawLockScreen();
            break;

        case 0x0E: // Backspace
            if (login_input_pos > 0) {
                login_input_pos--;
                login_input[login_input_pos] = '\0';
                drawLockScreen();
            }
            break;

        case 0x3A: // Caps Lock
            caps_lock_on = !caps_lock_on;
            drawLockScreen();
            break;

        default:
            // Handle alphanumeric input
            if (key >= 0x02 && key <= 0x0D) { // Numbers 1-0
                if (login_input_pos < MAX_PIN_LENGTH) {
                    char c = '1' + (key - 0x02);
                    if (key == 0x0B) c = '0'; // Handle 0 key
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x10 && key <= 0x19) { // Letters Q-P
                if (current_auth_mode == AUTH_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'q' + (key - 0x10);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x1E && key <= 0x26) { // Letters A-L
                if (current_auth_mode == AUTH_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'a' + (key - 0x1E);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x2C && key <= 0x32) { // Letters Z-M
                if (current_auth_mode == AUTH_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'z' + (key - 0x2C);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            }
            break;
    }
}

bool SecurityManager::isAuthenticated() {
    return security_authenticated;
}

void SecurityManager::logout() {
    security_authenticated = false;
    lockSystem();
}

void SecurityManager::resetFailedAttempts() {
    failed_login_attempts = 0;
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

void SecurityManager::drawLoginScreen() {
    showLoginScreen();
}

void SecurityManager::clearLoginInput() {
    login_input_pos = 0;
    custom_memset(login_input, 0, sizeof(login_input));
}

void SecurityManager::drawSecurityStatus() {
    // Implementation for security status display
}

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

struct User* AuthSystem::findUser(const char* username) {
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
    system_locked = true;
    is_authenticated = false;
    lock_screen_visible = true;

    // Create default admin user
    createUser("Administrator", ADMIN_DEFAULT_PASSWORD, ADMIN_DEFAULT_PIN, true);

    // Set default user avatar
    user_avatar = current_user[0];

    logSecurityEvent("System initialized", "SYSTEM");
}

bool AuthSystem::showLockScreen() {
    lock_screen_visible = true;
    login_input_pos = 0;
    custom_memset(login_input, 0, sizeof(login_input));

    drawLockScreen();
    return true;
}

void AuthSystem::handleLockScreenInput(uint8_t key) {
    if (!lock_screen_visible) return;

    switch (key) {
        case 0x1C: // Enter
            if (login_input_pos > 0) {
                login_input[login_input_pos] = '\0';
                int result;

                if (system_security_level == SECURITY_PIN) {
                    result = authenticatePin(login_input);
                } else {
                    result = authenticateUser(current_user, login_input);
                }

                if (result == AUTH_SUCCESS) {
                    lock_screen_visible = false;
                    system_locked = false;
                    // Clear screen for desktop
                    volatile char* video = (volatile char*)0xB8000;
                    for (int i = 0; i < 80 * 25 * 2; i += 2) {
                        video[i] = ' ';
                        video[i + 1] = 0x07;
                    }
                } else {
                    // Show error and reset
                    login_input_pos = 0;
                    custom_memset(login_input, 0, sizeof(login_input));
                    drawLockScreen();
                }
            }
            break;

        case 0x01: // Escape - Switch user (simplified)
            // For now, just toggle between PIN and password mode
            system_security_level = (system_security_level == SECURITY_PIN) ? 
                                   SECURITY_PASSWORD : SECURITY_PIN;
            login_input_pos = 0;
            custom_memset(login_input, 0, sizeof(login_input));
            drawLockScreen();
            break;

        case 0x0E: // Backspace
            if (login_input_pos > 0) {
                login_input_pos--;
                login_input[login_input_pos] = '\0';
                drawLockScreen();
            }
            break;

        case 0x3A: // Caps Lock
            caps_lock_on = !caps_lock_on;
            drawLockScreen();
            break;

        default:
            // Handle alphanumeric input
            if (key >= 0x02 && key <= 0x0D) { // Numbers 1-0
                if (login_input_pos < MAX_PIN_LENGTH) {
                    char c = '1' + (key - 0x02);
                    if (key == 0x0B) c = '0'; // Handle 0 key
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x10 && key <= 0x19) { // Letters Q-P
                if (system_security_level == SECURITY_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'q' + (key - 0x10);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x1E && key <= 0x26) { // Letters A-L
                if (system_security_level == SECURITY_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'a' + (key - 0x1E);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            } else if (key >= 0x2C && key <= 0x32) { // Letters Z-M
                if (system_security_level == SECURITY_PASSWORD && login_input_pos < MAX_PASSWORD_LENGTH) {
                    char c = 'z' + (key - 0x2C);
                    if (caps_lock_on) c = c - 'a' + 'A';
                    login_input[login_input_pos++] = c;
                    drawLockScreen();
                }
            }
            break;
    }
}

bool AuthSystem::isSystemLocked() {
    return system_locked;
}

bool AuthSystem::isLockScreenVisible() {
    return lock_screen_visible;
}

void AuthSystem::lockSystem() {
    system_locked = true;
    lock_screen_visible = true;
    is_authenticated = false;
    logSecurityEvent("System locked", "SYSTEM");
    drawLockScreen();
}

bool AuthSystem::isUserLocked(const char* username) {
    User* user = (User*)findUser(username);
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

int AuthSystem::authenticateUser(const char* username, const char* credential) {
    if (isSystemLocked()) {
        return AUTH_SYSTEM_LOCKED;
    }

    User* user = (User*)findUser(username);
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

int AuthSystem::authenticatePin(const char* pin) {
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
    if (findUser(username)) return false;

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
    User* user = (User*)findUser(username);
    if (user) {
        user->failed_attempts = 0;
        user->last_lockout_time = 0;
    }
}

void AuthSystem::unlockSystem() {
    system_locked = false;
    lock_screen_visible = false;
    logSecurityEvent("System unlocked", "SYSTEM");
}

void AuthSystem::logSecurityEvent(const char* event, const char* username) {
    if (log_count >= MAX_LOG_ENTRIES) {
        for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) {
            custom_strcpy(security_log[i], security_log[i + 1]);
        }
        log_count = MAX_LOG_ENTRIES - 1;
    }

    char* log_entry = security_log[log_count++];
    uint32_t time = getCurrentTime();

    int pos = 0;
    log_entry[pos++] = '[';
    log_entry[pos++] = '0' + (time / 1000) % 10;
    log_entry[pos++] = '0' + (time / 100) % 10;
    log_entry[pos++] = '0' + (time / 10) % 10;
    log_entry[pos++] = '0' + time % 10;
    log_entry[pos++] = ']';
    log_entry[pos++] = ' ';

    int event_len = custom_strlen(event);
    for (int i = 0; i < event_len && pos < 70; i++) {
        log_entry[pos++] = event[i];
    }

    log_entry[pos++] = ' ';
    log_entry[pos++] = '-';
    log_entry[pos++] = ' ';

    int user_len = custom_strlen(username);
    for (int i = 0; i < user_len && pos < 79; i++) {
        log_entry[pos++] = username[i];
    }

    log_entry[pos] = '\0';
}

bool AuthSystem::hasAdminPrivileges(const char* username) {
    User* user = (User*)findUser(username);
    return user && user->is_admin && is_authenticated;
}

int AuthSystem::getSystemSecurityLevel() {
    return system_security_level;
}

void AuthSystem::setSystemSecurityLevel(int level) {
    system_security_level = level;
    logSecurityEvent("Security level changed", current_user);
}

void AuthSystem::showSecurityLog() {
    // Implementation for security log display
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

bool AuthSystem::showLoginScreen() {
    return showLockScreen();
}

bool AuthSystem::showPinScreen() {
    system_security_level = SECURITY_PIN;
    return showLockScreen();
}

void AuthSystem::handleSecurityInput(uint8_t key) {
    handleLockScreenInput(key);
}
// The code has been fixed by applying type casting where necessary to avoid compilation errors.