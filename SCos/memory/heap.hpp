#pragma once

#include "../include/stddef.h"
#include "../include/memory.h"

bool init_heap();
void* krealloc(void* ptr, size_t size);
void heap_stats();