#pragma once

#include "../include/stddef.h"

bool init_heap();
void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);
void heap_stats();