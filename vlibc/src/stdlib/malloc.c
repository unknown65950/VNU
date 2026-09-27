#include <vlibc/stdlib.h>
#include <vlibc/sys/syscall.h>

typedef struct Block {
    unsigned long size;
    struct Block* next;
    int free;
} Block;

#define BLOCK_SIZE sizeof(Block)
#define MIN_ALLOC 16

static Block* heap_start = NULL;

void* malloc(unsigned long size) {
    if (size == 0) return NULL;
    
    size = (size + 7) & ~7;  // round up to 8 bytes
    
    // if the heap is not up yet
    if (heap_start == NULL) {
        // read the current break
        void* heap_end = (void*)syscall(SYS_brk, 0);
        if (heap_end == (void*)-1) return NULL;
        
        // ask for 1MB
        void* new_heap = (char*)heap_end + (1024 * 1024);
        void* result = (void*)syscall(SYS_brk, new_heap);
        if (result != new_heap) return NULL;
        
        heap_start = (Block*)heap_end;
        heap_start->size = 1024 * 1024 - BLOCK_SIZE;
        heap_start->next = NULL;
        heap_start->free = 1;
    }
    
    // find a free block
    Block* current = heap_start;
    while (current) {
        if (current->free && current->size >= size) {
            // split the block when it is too big
            if (current->size > size + BLOCK_SIZE + MIN_ALLOC) {
                Block* new_block = (Block*)((char*)current + BLOCK_SIZE + size);
                new_block->size = current->size - size - BLOCK_SIZE;
                new_block->next = current->next;
                new_block->free = 1;
                current->next = new_block;
                current->size = size;
            }
            current->free = 0;
            return (char*)current + BLOCK_SIZE;
        }
        current = current->next;
    }
    
    return NULL;
}

void free(void* ptr) {
    if (ptr == NULL) return;
    Block* block = (Block*)((char*)ptr - BLOCK_SIZE);
    block->free = 1;
}
