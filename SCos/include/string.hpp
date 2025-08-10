#ifndef STRING_HPP
#define STRING_HPP

#include "stddef.h"

extern "C" {
    
    void* memset(void* ptr, int value, size_t size);
    void* memcpy(void* dest, const void* src, size_t size);
    int memcmp(const void* ptr1, const void* ptr2, size_t size);

    
    size_t strlen(const char* str);
    char* strcpy(char* dest, const char* src);
    int strcmp(const char* str1, const char* str2);
}

#endif
