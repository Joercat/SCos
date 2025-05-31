
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char* str);
int strcmp(const char* str1, const char* str2);
int strncmp(const char* str1, const char* str2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);
int snprintf(char* buffer, size_t size, const char* format, ...);
int sscanf(const char* str, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif
