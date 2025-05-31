
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char* str);
int strcmp(const char* str1, const char* str2);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int snprintf(char* buffer, size_t size, const char* format, ...);
int sscanf(const char* str, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif
