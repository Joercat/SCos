
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
