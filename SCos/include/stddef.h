
#pragma once

// Include stdint.h first to get the basic integer types
#include "stdint.h"

typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;

#define NULL ((void*)0)

#ifdef __cplusplus
// Use built-in nullptr in C++
#else
#define nullptr NULL
#endif

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* ptr, int value, size_t size);
void* memcpy(void* dest, const void* src, size_t size);
int memcmp(const void* ptr1, const void* ptr2, size_t size);
size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
int strcmp(const char* str1, const char* str2);
int strncmp(const char* str1, const char* str2, size_t n);

#ifdef __cplusplus
}
#endif
