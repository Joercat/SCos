
#ifndef SECURITY_CENTER_HPP
#define SECURITY_CENTER_HPP

#include <stdint.h>

void openSecurityCenter();

class SecurityCenter {
public:
    static void handleInput(uint8_t key);
};

#endif
