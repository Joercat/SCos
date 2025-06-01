
#include "ramfs.hpp"
#include <stdint.h>

// Custom string functions for freestanding environment
static int custom_strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static int custom_strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

static void custom_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// File structure for RAM filesystem
struct File {
    char path[256];
    char content[1024];
    bool in_use;
};

// Maximum number of files in the filesystem
#define MAX_FILES 16

static File files[MAX_FILES];
static int fileCount = 0;
static bool fs_initialized = false;

bool initFS() {
    // Clear all file entries
    for (int i = 0; i < MAX_FILES; i++) {
        files[i].in_use = false;
        files[i].path[0] = '\0';
        files[i].content[0] = '\0';
    }
    
    fileCount = 0;
    fs_initialized = true;
    
    // Create some default files
    writeFile("/home/welcome.txt", "Welcome to SCos Notepad!\nThis is a simple text editor for SCos.");
    writeFile("/home/readme.txt", "SCos Operating System\nVersion 0.1.0\n\nBasic filesystem operations are now available.");
    writeFile("/system/version.txt", "SCos v0.1.0");
    writeFile("/temp/test.txt", "Temporary file for testing");
    
    return true;
}

const char* readFile(const char* path) {
    if (!fs_initialized) {
        return "(filesystem not initialized)";
    }
    
    if (!path) {
        return "(invalid path)";
    }
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].in_use && custom_strcmp(files[i].path, path) == 0) {
            return files[i].content;
        }
    }
    
    return "(file not found)";
}

bool writeFile(const char* path, const char* data) {
    if (!fs_initialized) {
        return false;
    }
    
    if (!path || !data) {
        return false;
    }
    
    // Check if file already exists and update it
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].in_use && custom_strcmp(files[i].path, path) == 0) {
            // Update existing file
            int len = custom_strlen(data);
            if (len >= 1024) len = 1023; // Prevent buffer overflow
            
            for (int j = 0; j < len; j++) {
                files[i].content[j] = data[j];
            }
            files[i].content[len] = '\0';
            return true;
        }
    }
    
    // Create new file if space available
    if (fileCount < MAX_FILES) {
        // Find first available slot
        for (int i = 0; i < MAX_FILES; i++) {
            if (!files[i].in_use) {
                files[i].in_use = true;
                
                // Copy path
                int path_len = custom_strlen(path);
                if (path_len >= 256) path_len = 255;
                for (int j = 0; j < path_len; j++) {
                    files[i].path[j] = path[j];
                }
                files[i].path[path_len] = '\0';
                
                // Copy content
                int data_len = custom_strlen(data);
                if (data_len >= 1024) data_len = 1023;
                for (int j = 0; j < data_len; j++) {
                    files[i].content[j] = data[j];
                }
                files[i].content[data_len] = '\0';
                
                fileCount++;
                return true;
            }
        }
    }
    
    return false; // No space available
}

// Additional utility functions for filesystem operations
bool deleteFile(const char* path) {
    if (!fs_initialized || !path) {
        return false;
    }
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].in_use && custom_strcmp(files[i].path, path) == 0) {
            files[i].in_use = false;
            files[i].path[0] = '\0';
            files[i].content[0] = '\0';
            fileCount--;
            return true;
        }
    }
    
    return false;
}

bool fileExists(const char* path) {
    if (!fs_initialized || !path) {
        return false;
    }
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].in_use && custom_strcmp(files[i].path, path) == 0) {
            return true;
        }
    }
    
    return false;
}

int getFileCount() {
    return fileCount;
}
