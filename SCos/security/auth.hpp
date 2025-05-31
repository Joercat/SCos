
#ifndef AUTH_HPP
#define AUTH_HPP

#include <stdint.h>

#define MAX_PIN_LENGTH 8
#define MAX_PASSWORD_LENGTH 16
#define MAX_USERS 5
#define LOCKOUT_ATTEMPTS 3
#define LOCKOUT_TIME 30 // seconds

enum SecurityLevel {
    SECURITY_NONE = 0,
    SECURITY_PIN = 1,
    SECURITY_PASSWORD = 2,
    SECURITY_BIOMETRIC = 3
};

enum AuthResult {
    AUTH_SUCCESS = 0,
    AUTH_INVALID_CREDENTIALS = 1,
    AUTH_ACCOUNT_LOCKED = 2,
    AUTH_SYSTEM_LOCKED = 3,
    AUTH_TIMEOUT = 4
};

struct User {
    char username[16];
    char password[MAX_PASSWORD_LENGTH + 1];
    char pin[MAX_PIN_LENGTH + 1];
    SecurityLevel security_level;
    bool is_admin;
    bool is_active;
    int failed_attempts;
    uint32_t last_lockout_time;
};

class AuthSystem {
public:
    static void init();
    static bool isSystemLocked();
    static bool isUserLocked(const char* username);
    static AuthResult authenticateUser(const char* username, const char* credential);
    static AuthResult authenticatePin(const char* pin);
    static bool createUser(const char* username, const char* password, const char* pin, bool is_admin);
    static bool changePassword(const char* username, const char* old_password, const char* new_password);
    static bool changePin(const char* username, const char* old_pin, const char* new_pin);
    static void lockUser(const char* username);
    static void unlockUser(const char* username);
    static void lockSystem();
    static void unlockSystem();
    static bool hasAdminPrivileges(const char* username);
    static void logSecurityEvent(const char* event, const char* username);
    static void showSecurityLog();
    static SecurityLevel getSystemSecurityLevel();
    static void setSystemSecurityLevel(SecurityLevel level);
    
    // Security UI
    static bool showLoginScreen();
    static bool showPinScreen();
    static void showSecuritySettings();
    static void handleSecurityInput(uint8_t key);
    
private:
    static void hashPassword(const char* password, char* hash);
    static bool verifyPassword(const char* password, const char* hash);
    static void clearFailedAttempts(const char* username);
    static uint32_t getCurrentTime();
    static User* findUser(const char* username);
};

// Security constants
#define ADMIN_DEFAULT_PIN "1337"
#define ADMIN_DEFAULT_PASSWORD "admin123"
#define SYSTEM_DEFAULT_PIN "0000"

#endif
