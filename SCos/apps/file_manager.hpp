#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <stdint.h>

class FileManager {
public:
    static void handleInput(uint8_t key);
};

void openFileManager();

#endif