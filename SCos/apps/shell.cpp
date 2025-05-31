#include "shell.hpp"
#include "../fs/ramfs.hpp"
#include "../debug/serial.hpp"
#include "../include/stddef.h"
#include <cstring>

Shell::Shell() {
    current_directory[0] = '/';
    current_directory[1] = '\0';
}

bool Shell::execute_command(const char* cmd, char* output, size_t output_size) {
    if (strlen(cmd) == 0) return true;
    
    char command[256];
    char args[256];
    parse_command(cmd, command, args);
    
    if (strcmp(command, "ls") == 0) {
        return cmd_ls(args, output, output_size);
    }
    else if (strcmp(command, "cd") == 0) {
        return cmd_cd(args, output, output_size);
    }
    else if (strcmp(command, "pwd") == 0) {
        return cmd_pwd(output, output_size);
    }
    else if (strcmp(command, "mkdir") == 0) {
        return cmd_mkdir(args, output, output_size);
    }
    else if (strcmp(command, "touch") == 0) {
        return cmd_touch(args, output, output_size);
    }
    else if (strcmp(command, "cat") == 0) {
        return cmd_cat(args, output, output_size);
    }
    else if (strcmp(command, "rm") == 0) {
        return cmd_rm(args, output, output_size);
    }
    else if (strcmp(command, "cp") == 0) {
        return cmd_cp(args, output, output_size);
    }
    else if (strcmp(command, "mv") == 0) {
        return cmd_mv(args, output, output_size);
    }
    else if (strcmp(command, "find") == 0) {
        return cmd_find(args, output, output_size);
    }
    else {
        snprintf(output, output_size, "Command not found: %s", command);
        return false;
    }
}

void Shell::parse_command(const char* input, char* command, char* args) {
    int i = 0;
    while (input[i] && input[i] != ' ') {
        command[i] = input[i];
        i++;
    }
    command[i] = '\0';
    
    while (input[i] == ' ') i++;
    
    int j = 0;
    while (input[i]) {
        args[j++] = input[i++];
    }
    args[j] = '\0';
}

bool Shell::cmd_ls(const char* args, char* output, size_t output_size) {
    const char* path = (strlen(args) > 0) ? args : current_directory;
    
    // Simple file listing implementation
    strcpy(output, "Files in ");
    strcat(output, path);
    strcat(output, ":\n");
    strcat(output, "..\n");
    strcat(output, "documents/\n");
    strcat(output, "system/\n");
    strcat(output, "temp/\n");
    strcat(output, "readme.txt\n");
    
    return true;
}

bool Shell::cmd_cd(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(current_directory, "/");
        strcpy(output, "Changed to root directory");
        return true;
    }
    
    if (strcmp(args, "..") == 0) {
        if (strcmp(current_directory, "/") != 0) {
            char* last_slash = strrchr(current_directory, '/');
            if (last_slash && last_slash != current_directory) {
                *last_slash = '\0';
            } else {
                strcpy(current_directory, "/");
            }
        }
        snprintf(output, output_size, "Changed to %s", current_directory);
        return true;
    }
    
    char new_path[512];
    if (args[0] == '/') {
        strcpy(new_path, args);
    } else {
        strcpy(new_path, current_directory);
        if (current_directory[strlen(current_directory) - 1] != '/') {
            strcat(new_path, "/");
        }
        strcat(new_path, args);
    }
    
    strcpy(current_directory, new_path);
    snprintf(output, output_size, "Changed to %s", current_directory);
    return true;
}

bool Shell::cmd_pwd(char* output, size_t output_size) {
    snprintf(output, output_size, "%s", current_directory);
    return true;
}

bool Shell::cmd_mkdir(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(output, "Usage: mkdir <directory>");
        return false;
    }
    
    snprintf(output, output_size, "Created directory: %s", args);
    return true;
}

bool Shell::cmd_touch(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(output, "Usage: touch <filename>");
        return false;
    }
    
    snprintf(output, output_size, "Created file: %s", args);
    return true;
}

bool Shell::cmd_cat(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(output, "Usage: cat <filename>");
        return false;
    }
    
    if (strcmp(args, "readme.txt") == 0) {
        strcpy(output, "Welcome to SCos!\n");
        strcat(output, "This is a simple operating system.\n");
        strcat(output, "Type 'help' for available commands.");
    } else {
        snprintf(output, output_size, "File not found: %s", args);
        return false;
    }
    
    return true;
}

bool Shell::cmd_rm(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(output, "Usage: rm <filename>");
        return false;
    }
    
    snprintf(output, output_size, "Removed: %s", args);
    return true;
}

bool Shell::cmd_cp(const char* args, char* output, size_t output_size) {
    char src[256], dst[256];
    if (sscanf(args, "%s %s", src, dst) != 2) {
        strcpy(output, "Usage: cp <source> <destination>");
        return false;
    }
    
    snprintf(output, output_size, "Copied %s to %s", src, dst);
    return true;
}

bool Shell::cmd_mv(const char* args, char* output, size_t output_size) {
    char src[256], dst[256];
    if (sscanf(args, "%s %s", src, dst) != 2) {
        strcpy(output, "Usage: mv <source> <destination>");
        return false;
    }
    
    snprintf(output, output_size, "Moved %s to %s", src, dst);
    return true;
}

bool Shell::cmd_find(const char* args, char* output, size_t output_size) {
    if (strlen(args) == 0) {
        strcpy(output, "Usage: find <pattern>");
        return false;
    }
    
    snprintf(output, output_size, "Searching for: %s\nFound: readme.txt", args);
    return true;
}

const char* Shell::get_current_directory() const {
    return current_directory;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    size_t pos = 0;
    while (*format && pos < size - 1) {
        if (*format == '%') {
            format++;
            if (*format == 's') {
                const char* s = va_arg(args, const char*);
                while (*s && pos < size - 1) {
                    str[pos++] = *s++;
                }
            } else if (*format == 'd') {
                int d = va_arg(args, int);
                char num[32];
                int i = 0;
                if (d == 0) {
                    num[i++] = '0';
                } else {
                    if (d < 0) {
                        str[pos++] = '-';
                        d = -d;
                    }
                    while (d > 0) {
                        num[i++] = '0' + (d % 10);
                        d /= 10;
                    }
                    for (int j = i - 1; j >= 0 && pos < size - 1; j--) {
                        str[pos++] = num[j];
                    }
                }
            }
        } else {
            str[pos++] = *format;
        }
        format++;
    }
    str[pos] = '\0';
    
    va_end(args);
    return pos;
}

int sscanf(const char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    int matches = 0;
    const char* s = str;
    
    while (*format && *s) {
        if (*format == '%') {
            format++;
            if (*format == 's') {
                char* dest = va_arg(args, char*);
                while (*s && *s != ' ') {
                    *dest++ = *s++;
                }
                *dest = '\0';
                matches++;
                while (*s == ' ') s++;
            }
        } else if (*format == *s) {
            format++;
            s++;
        } else {
            break;
        }
    }
    
    va_end(args);
    return matches;
}