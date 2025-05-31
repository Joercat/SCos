#include "heap.hpp"
#include "../include/stddef.h"
#include "../debug/serial.hpp"
#include "../include/memory.h"

extern "C" {
    extern uint32_t _kernel_end;
    void* memcpy(void* dest, const void* src, size_t size);
}

static uint32_t heap_start;
static uint32_t heap_end;
static uint32_t heap_current;

struct heap_block {
    uint32_t size;
    bool used;
    heap_block* next;
};

static heap_block* first_block = nullptr;

bool init_heap() {
    heap_start = (_kernel_end + 0xFFF) & ~0xFFF;
    heap_end = heap_start + (4 * 1024 * 1024);
    heap_current = heap_start;
    
    first_block = (heap_block*)heap_start;
    first_block->size = heap_end - heap_start - sizeof(heap_block);
    first_block->used = false;
    first_block->next = nullptr;
    
    serial_printf("Heap initialized: 0x%x - 0x%x (%d KB)\n", 
                  heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    return true;
}

void* kmalloc(size_t size) {
    if (size == 0) return nullptr;
    
    // Align size to 4 bytes
    size = (size + 3) & ~3;
    
    heap_block* current = first_block;
    while (current) {
        if (!current->used && current->size >= size) {
            // Split block if it's larger than needed
            if (current->size > size + sizeof(heap_block) + 4) {
                heap_block* new_block = (heap_block*)((uint8_t*)current + sizeof(heap_block) + size);
                new_block->size = current->size - size - sizeof(heap_block);
                new_block->used = false;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->used = true;
            return (uint8_t*)current + sizeof(heap_block);
        }
        current = current->next;
    }
    
    return nullptr;
}

void* kmalloc(size_t size) {
    if (size == 0) return nullptr;
    
    size = (size + 3) & ~3;
    
    heap_block* current = first_block;
    while (current) {
        if (!current->used && current->size >= size) {
            if (current->size > size + sizeof(heap_block)) {
                heap_block* new_block = (heap_block*)((uint8_t*)current + sizeof(heap_block) + size);
                new_block->size = current->size - size - sizeof(heap_block);
                new_block->used = false;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            current->used = true;
            return (void*)((uint8_t*)current + sizeof(heap_block));
        }
        current = current->next;
    }
    
    return nullptr;
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    heap_block* block = (heap_block*)((uint8_t*)ptr - sizeof(heap_block));
    block->used = false;
    
    heap_block* current = first_block;
    while (current) {
        if (!current->used && current->next && !current->next->used) {
            current->size += current->next->size + sizeof(heap_block);
            current->next = current->next->next;
        }
        current = current->next;
    }
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return nullptr;
    }
    
    heap_block* block = (heap_block*)((uint8_t*)ptr - sizeof(heap_block));
    if (block->size >= size) {
        return ptr;
    }
    
    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size < size ? block->size : size);
        kfree(ptr);
    }
    return new_ptr;
}

void heap_stats() {
    uint32_t total_blocks = 0;
    uint32_t used_blocks = 0;
    uint32_t free_blocks = 0;
    uint32_t used_memory = 0;
    uint32_t free_memory = 0;
    
    heap_block* current = first_block;
    while (current) {
        total_blocks++;
        if (current->used) {
            used_blocks++;
            used_memory += current->size;
        } else {
            free_blocks++;
            free_memory += current->size;
        }
        current = current->next;
    }
    
    serial_printf("Heap Stats - Blocks: %d total, %d used, %d free\n", 
                  total_blocks, used_blocks, free_blocks);
    serial_printf("Memory: %d KB used, %d KB free\n", 
                  used_memory / 1024, free_memory / 1024);
}