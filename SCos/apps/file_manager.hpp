
#pragma once
#include "../ui/window_manager.hpp"
#include <stdint.h>

#define MAX_FILES 20
#define MAX_FILENAME_LENGTH 32

class FileManager {
public:
    static void init();
    static void show();
    static void hide();
    static bool isVisible();
    static void handleInput(uint8_t key);
    static void handleMouseClick(int x, int y);

private:
    static void drawFileManager();
    static void refreshFileList();
    static void selectFile(int index);
    static void openFile();
    static void createFile();
    static void deleteFile();
    static void updateDisplay();
};
