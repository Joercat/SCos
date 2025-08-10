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
    heap_start = (_kernel_end + 0x1000) & ~0xFFF;
    heap_end = heap_start + (1 * 1024 * 1024);
    heap_current = heap_start;

    if (heap_start >= 0x1000000) {
        serial_printf("ERROR: Heap start too high: 0x%x\n", heap_start);
        return false;
    }

    uint8_t* clear_ptr = (uint8_t*)heap_start;
    for (uint32_t i = 0; i < (heap_end - heap_start); i++) {
        clear_ptr[i] = 0;
    }

    first_block = (heap_block*)heap_start;
    first_block->size = heap_end - heap_start - sizeof(heap_block);
    first_block->used = false;
    first_block->next = nullptr;

    serial_printf("Heap initialized: 0x%x - 0x%x (%d KB)\n", 
                  heap_start, heap_end, (heap_end - heap_start) / 1024);
    serial_printf("Kernel end: 0x%x, Heap buffer: %d bytes\n", 
                  _kernel_end, heap_start - _kernel_end);

    return true;
}

void* kmalloc(size_t size) {
    if (size == 0) return static_cast<void*>(nullptr);
    
    if (size > (512 * 1024)) {
        serial_printf("WARNING: Large allocation requested: %d bytes\n", size);
        return static_cast<void*>(nullptr);
    }

    size = (size + 3) & ~3;

    heap_block* current = first_block;
    while (current) {
        if (!current->used && current->size >= size) {
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

    return static_cast<void*>(nullptr);
}

void kfree(void* ptr) {
    if (!ptr) return;

    if ((uint32_t)ptr < heap_start || (uint32_t)ptr >= heap_end) {
        serial_printf("ERROR: Invalid free - ptr 0x%x outside heap 0x%x-0x%x\n", 
                     (uint32_t)ptr, heap_start, heap_end);
        return;
    }

    heap_block* block = (heap_block*)((uint8_t*)ptr - sizeof(heap_block));
    
    if ((uint32_t)block < heap_start || (uint32_t)block >= heap_end) {
        serial_printf("ERROR: Invalid block header at 0x%x\n", (uint32_t)block);
        return;
    }
    
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
        return static_cast<void*>(nullptr);
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
