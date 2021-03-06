//----------------------------------------------------------------
// Statically-allocated memory manager
//
// by Eli Bendersky (eliben@gmail.com)
//  
// This code is in the public domain.
//----------------------------------------------------------------
#include <stdio.h>
#include "memmgr.h"

typedef ulong Align;

union mem_header_union
{
    struct 
    {
        // Pointer to the next block in the free list
        //
        union mem_header_union* next;

        // Size of the block (in quantas of sizeof(mem_header_t))
        //
        ulong size; 
    } s;

    // Used to align headers in memory to a boundary
    //
    Align align_dummy;
};

typedef union mem_header_union mem_header_t;

struct arena {
    // Initial empty list
    //
    mem_header_t base;

    // Start of free list
    //
    mem_header_t* freep;

    // Static pool for new allocations
    //
    byte pool[POOL_SIZE];
    ulong pool_free_pos;
};

#ifdef ELFBAC
#define STATE(state) __attribute__((section(state)))
#else
#define STATE(state)
#endif

STATE(".data.crypto_heap") struct arena crypto_arena;
STATE(".data.roaming_heap") struct arena roaming_arena;
STATE(".data.packet_heap") struct arena packet_arena;
STATE(".data.shared_heap") struct arena shared_arena;

static struct arena *arenas[] = {
    &shared_arena,
    &packet_arena,
    &roaming_arena,
    &crypto_arena
};

#if 0
void memmgr_init()
{
    unsigned int i;
    for (i = 0; i < NUM_ARENAS; i++) {
        arenas[i]->base.s.next = 0;
        arenas[i]->base.s.size = 0;
        arenas[i]->freep = 0;
        arenas[i]->pool_free_pos = 0;
    }
}

void memmgr_print_stats()
{
    #ifdef DEBUG_MEMMGR_SUPPORT_STATS
    mem_header_t* p;

    printf("------ Memory manager stats ------\n\n");
    printf(    "Pool: free_pos = %lu (%lu bytes left)\n\n", 
            pool_free_pos, POOL_SIZE - pool_free_pos);

    p = (mem_header_t*) pool;

    while (p < (mem_header_t*) (pool + pool_free_pos))
    {
        printf(    "  * Addr: 0x%8lu; Size: %8lu\n",
                p, p->s.size);

        p += p->s.size;
    }

    printf("\nFree list:\n\n");

    if (freep)
    {
        p = freep;

        while (1)
        {
            printf(    "  * Addr: 0x%8lu; Size: %8lu; Next: 0x%8lu\n", 
                    p, p->s.size, p->s.next);

            p = p->s.next;

            if (p == freep)
                break;
        }
    }
    else
    {
        printf("Empty\n");
    }
    
    printf("\n");

    #endif // DEBUG_MEMMGR_SUPPORT_STATS
}
#endif


static mem_header_t* get_mem_from_pool(struct arena *arena, ulong nquantas)
{
    ulong total_req_size;

    mem_header_t* h;

    if (nquantas < MIN_POOL_ALLOC_QUANTAS)
        nquantas = MIN_POOL_ALLOC_QUANTAS;

    total_req_size = nquantas * sizeof(mem_header_t);

    // Introduce a bug to allow for memory disclosure from overlong malloc in
    // our split heap implementation
//    if (arena->pool_free_pos + total_req_size <= POOL_SIZE)
//    {
        h = (mem_header_t*) (arena->pool + arena->pool_free_pos);
        h->s.size = nquantas;
        memmgr_free((void*) (h + 1));
        arena->pool_free_pos += total_req_size;
//    }
//    else
//    {
//        return 0;
//    }

    return arena->freep;
}


// Allocations are done in 'quantas' of header size.
// The search for a free block of adequate size begins at the point 'freep' 
// where the last block was found.
// If a too-big block is found, it is split and the tail is returned (this 
// way the header of the original needs only to have its size adjusted).
// The pointer returned to the user points to the free space within the block,
// which begins one quanta after the header.
//
void* memmgr_alloc(ulong nbytes, enum arenas arena_idx)
{
    mem_header_t* p;
    mem_header_t* prevp;
    struct arena *arena = arenas[arena_idx];

    // Calculate how many quantas are required: we need enough to house all
    // the requested bytes, plus the header. The -1 and +1 are there to make sure
    // that if nbytes is a multiple of nquantas, we don't allocate too much
    //
    ulong nquantas = (nbytes + sizeof(mem_header_t) - 1) / sizeof(mem_header_t) + 1;

    // First alloc call, and no free list yet ? Use 'base' for an initial
    // denegerate block of size 0, which points to itself
    // 
    if ((prevp = arena->freep) == 0)
    {
        arena->base.s.next = arena->freep = prevp = &arena->base;
        arena->base.s.size = 0;
    }

    for (p = prevp->s.next; ; prevp = p, p = p->s.next)
    {
        // big enough ?
        if (p->s.size >= nquantas) 
        {
            // exactly ?
            if (p->s.size == nquantas)
            {
                // just eliminate this block from the free list by pointing
                // its prev's next to its next
                //
                prevp->s.next = p->s.next;
            }
            else // too big
            {
                p->s.size -= nquantas;
                p += p->s.size;
                p->s.size = nquantas;
            }

            arena->freep = prevp;
            return (void*) (p + 1);
        }
        // Reached end of free list ?
        // Try to allocate the block from the pool. If that succeeds,
        // get_mem_from_pool adds the new block to the free list and
        // it will be found in the following iterations. If the call
        // to get_mem_from_pool doesn't succeed, we've run out of
        // memory
        //
        else if (p == arena->freep)
        {
            if ((p = get_mem_from_pool(arena, nquantas)) == 0)
            {
                #ifdef DEBUG_MEMMGR_FATAL
                printf("!! Memory allocation failed !!\n");
                #endif
                return 0;
            }
        }
    }
}


// Scans the free list, starting at freep, looking the the place to insert the 
// free block. This is either between two existing blocks or at the end of the
// list. In any case, if the block being freed is adjacent to either neighbor,
// the adjacent blocks are combined.
//
void memmgr_free(void* ap)
{
    mem_header_t* block;
    mem_header_t* p;
    unsigned int i;
    struct arena *arena = NULL;

    for (i = 0; i < NUM_ARENAS; i++) {
        if ((byte *)ap >= arenas[i]->pool && (byte *)ap < (arenas[i]->pool + POOL_SIZE)) {
            arena = arenas[i];
            break;
        }
    }

    if (!arena)
        return;

    // acquire pointer to block header
    block = ((mem_header_t*) ap) - 1;

    // Find the correct place to place the block in (the free list is sorted by
    // address, increasing order)
    //
    for (p = arena->freep; !(block > p && block < p->s.next); p = p->s.next)
    {
        // Since the free list is circular, there is one link where a 
        // higher-addressed block points to a lower-addressed block. 
        // This condition checks if the block should be actually 
        // inserted between them
        //
        if (p >= p->s.next && (block > p || block < p->s.next))
            break;
    }

    // Try to combine with the higher neighbor
    //
    if (block + block->s.size == p->s.next)
    {
        block->s.size += p->s.next->s.size;
        block->s.next = p->s.next->s.next;
    }
    else
    {
        block->s.next = p->s.next;
    }

    // Try to combine with the lower neighbor
    //
    if (p + p->s.size == block)
    {
        p->s.size += block->s.size;
        p->s.next = block->s.next;
    }
    else
    {
        p->s.next = block;
    }

    arena->freep = p;
}
