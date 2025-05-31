#pragma once
#include "../include/stddef.h"
#include <stdarg.h>

class Shell {
private:
    char current_directory[512];
    
    void parse_command(const char* input, char* command, char* args);
    
    bool cmd_ls(const char* args, char* output, size_t output_size);
    bool cmd_cd(const char* args, char* output, size_t output_size);
    bool cmd_pwd(char* output, size_t output_size);
    bool cmd_mkdir(const char* args, char* output, size_t output_size);
    bool cmd_touch(const char* args, char* output, size_t output_size);
    bool cmd_cat(const char* args, char* output, size_t output_size);
    bool cmd_rm(const char* args, char* output, size_t output_size);
    bool cmd_cp(const char* args, char* output, size_t output_size);
    bool cmd_mv(const char* args, char* output, size_t output_size);
    bool cmd_find(const char* args, char* output, size_t output_size);
    
public:
    Shell();
    bool execute_command(const char* cmd, char* output, size_t output_size);
    const char* get_current_directory() const;
};

int snprintf(char* str, size_t size, const char* format, ...);
int sscanf(const char* str, const char* format, ...);