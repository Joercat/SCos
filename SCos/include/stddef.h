#pragma once

typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;

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

#ifdef __cplusplus
}
#endif