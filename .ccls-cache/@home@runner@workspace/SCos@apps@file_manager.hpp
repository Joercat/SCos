
#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

// Function declarations for file manager application
void openFileManager();

// FileManager class for input handling
class FileManager {
public:
    static void handleInput(char key);
};

#endif
#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <stdint.h>

class FileManager {
public:
    static void handleInput(uint8_t key);
};

void openFileManager();

#endif
