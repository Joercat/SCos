
#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* kmalloc(size_t size);
void kfree(void* ptr);
void* memset(void* dest, int value, size_t count);
void* memcpy(void* dest, const void* src, size_t count);

#ifdef __cplusplus
}
#endif

#endif
