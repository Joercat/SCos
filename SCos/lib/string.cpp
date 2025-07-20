
#include "../include/string.hpp"

extern "C" {
void* memset(void* ptr, int value, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    while (size--) {
        *p++ = (unsigned char)value;
    }
    return ptr;
}

void* memcpy(void* dest, const void* src, size_t size) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (size--) {
        *d++ = *s++;
    }
    return dest;
}

int memcmp(const void* ptr1, const void* ptr2, size_t size) {
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    while (size--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* orig_dest = dest;
    while ((*dest++ = *src++));
    return orig_dest;
}

int strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(unsigned char*)str1 - *(unsigned char*)str2;
}

char* strrchr(const char* str, int c) {
    const char* last = nullptr;
    while (*str) {
        if (*str == c) {
            last = str;
        }
        str++;
    }
    if (c == '\0') {
        return (char*)str;
    }
    return (char*)last;
}

int strncmp(const char* str1, const char* str2, size_t n) {
    while (n && *str1 && (*str1 == *str2)) {
        ++str1;
        ++str2;
        --n;
    }
    if (n == 0) {
        return 0;
    } else {
        return (*(unsigned char*)str1 - *(unsigned char*)str2);
    }
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;

    for (const char* h = haystack; *h; h++) {
        const char* h_temp = h;
        const char* n_temp = needle;

        while (*h_temp && *n_temp && *h_temp == *n_temp) {
            h_temp++;
            n_temp++;
        }

        if (!*n_temp) return (char*)h;
    }
    return (char*)0;
}

char* strcat(char* dest, const char* src) {
    char* orig_dest = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return orig_dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}
}
