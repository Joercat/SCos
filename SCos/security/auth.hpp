
#ifndef AUTH_HPP
#define AUTH_HPP

#include <stdint.h>

// Authentication modes
enum AuthMode {
    AUTH_NONE = 0,
    AUTH_PIN = 1,
    AUTH_PASSWORD = 2,
    AUTH_PIN_PASSWORD = 3
};

#define MAX_PIN_LENGTH 8
#define MAX_PASSWORD_LENGTH 32
#define MAX_USERNAME_LENGTH 16

enum AuthResult {
    AUTH_SUCCESS = 0,
    AUTH_FAILED = 1,
    AUTH_LOCKED = 2,
    AUTH_TIMEOUT = 3
};

struct UserProfile {
    char username[MAX_USERNAME_LENGTH];
    char pin[MAX_PIN_LENGTH + 1];
    char password_hash[33]; // MD5 hash as string
    bool is_admin;
    int failed_attempts;
    bool locked;
    uint32_t last_login_time;
};

class SecurityManager {
public:
    static bool init();
    static bool authenticate(const char* input, AuthMode mode);
    static void lockSystem();
    static void unlockSystem();
    static bool isSystemLocked();
    static bool changePin(const char* old_pin, const char* new_pin);
    static bool changePassword(const char* old_password, const char* new_password);
    static void showLoginScreen();
    static void handleLoginInput(uint8_t key);
    static bool isAuthenticated();
    static void logout();
    static void resetFailedAttempts();
    static int getFailedAttempts();
    static void setAuthMode(AuthMode mode);
    static AuthMode getAuthMode();

private:
    static void drawLoginScreen();
    static void clearLoginInput();
    static uint32_t simpleHash(const char* str);
    static bool verifyHash(const char* input, const char* stored_hash);
    static void drawSecurityStatus();
};

// AuthSystem class for desktop compatibility
class AuthSystem {
public:
    static void init();
    static bool isLockScreenVisible();
    static void handleLockScreenInput(uint8_t key);
    static bool isAuthenticated();
    static void lockSystem();
    static void unlockSystem();
    static bool isSystemLocked();
    static bool showLockScreen();
    static bool showLoginScreen();
    static bool showPinScreen();
    static void handleSecurityInput(uint8_t key);
    
    // Additional methods for full AuthSystem functionality
    static void hashPassword(const char* password, char* hash);
    static bool verifyPassword(const char* password, const char* stored_hash);
    static uint32_t getCurrentTime();
    static void* findUser(const char* username);
    static bool isUserLocked(const char* username);
    static int authenticateUser(const char* username, const char* credential);
    static int authenticatePin(const char* pin);
    static bool createUser(const char* username, const char* password, const char* pin, bool is_admin);
    static void clearFailedAttempts(const char* username);
    static void logSecurityEvent(const char* event, const char* username);
    static bool hasAdminPrivileges(const char* username);
    static int getSystemSecurityLevel();
    static void setSystemSecurityLevel(int level);
    static void showSecurityLog();
};

#endif
