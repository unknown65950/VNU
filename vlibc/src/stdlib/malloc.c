/*
 * malloc.c — the freestanding heap allocator.
 *
 * A single address-ordered free list over a region that grows on
 * demand: whenever no free block can satisfy a request, the allocator
 * coalesces neighbouring free blocks and, if that still is not enough,
 * asks the kernel for another lump through brk(). That makes the heap
 * unbounded up to whatever the address space allows, instead of the old
 * fixed 1 MiB arena that silently returned NULL once a program's live
 * set plus its fragmentation reached the cap.
 *
 * The list stays in address order because lumps are only ever appended
 * at the (higher) break and a split inserts immediately after the block
 * being split. free() is O(1); coalescing runs only on the allocation
 * slow path, so the common case never pays for it.
 */
#include <vlibc/stdlib.h>
#include <vlibc/sys/syscall.h>

typedef struct Block {
    unsigned long size;
    struct Block* next;
    int free;
} Block;

#define BLOCK_SIZE sizeof(Block)
#define MIN_ALLOC 16

/* The first lump a fresh heap asks for; kept at 1 MiB so that a program
 * which allocates anything at all gets the same comfortable start it
 * always did. Larger requests round up to the next power of two. */
#define HEAP_LUMP (1024 * 1024)

static Block* heap_start = NULL;

/* Ask the kernel to extend the break by a lump big enough for `need`
 * bytes plus a header, and append the new block to the free list.
 * Returns 0 when the kernel refuses (out of address space / memory). */
static int more_heap(unsigned long need) {
    void* brk = (void*)syscall(SYS_brk, 0);
    if (brk == (void*)-1)
        return 0;

    unsigned long cur = (unsigned long)brk;
    unsigned long grow = HEAP_LUMP;
    while (grow < need + BLOCK_SIZE + MIN_ALLOC)
        grow <<= 1;

    void* want = (void*)(cur + grow);
    void* got = (void*)syscall(SYS_brk, want);
    if (got != want)
        return 0;

    Block* b = (Block*)cur;
    b->size = grow - BLOCK_SIZE;
    b->next = NULL;
    b->free = 1;

    if (heap_start == NULL) {
        heap_start = b;
    } else {
        Block* c = heap_start;
        while (c->next)
            c = c->next;
        c->next = b;
    }
    return 1;
}

/* Merge every pair of adjacent free blocks. The list is in address
 * order, so a single pass keeps it fully coalesced. */
static void coalesce(void) {
    Block* b = heap_start;
    while (b && b->next) {
        if (b->free && b->next->free) {
            b->size += BLOCK_SIZE + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}

static Block* find_block(unsigned long size) {
    Block* current = heap_start;
    while (current) {
        if (current->free && current->size >= size)
            return current;
        current = current->next;
    }
    return NULL;
}

static void* take(Block* current, unsigned long size) {
    /* Split the block when the remainder can hold a header and the
     * minimum useful payload. */
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

void* malloc(unsigned long size) {
    if (size == 0)
        return NULL;

    size = (size + 7) & ~7UL; /* round up to 8 bytes */

    int coalesced = 0;
    for (;;) {
        Block* block = find_block(size);
        if (block)
            return take(block, size);
        /* No block fit. Merge neighbouring free blocks once and retry;
         * if that still is not enough, ask the kernel for another lump. */
        if (!coalesced) {
            coalesce();
            coalesced = 1;
            continue;
        }
        if (!more_heap(size))
            return NULL;
        coalesced = 0;
    }
}

void free(void* ptr) {
    if (ptr == NULL)
        return;
    Block* block = (Block*)((char*)ptr - BLOCK_SIZE);
    block->free = 1;
}
