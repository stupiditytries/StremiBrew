/*
 * Host test for heap.c: builds the heap under different names against stand-ins for the
 * console's direct-memory calls, then runs a randomised allocate/free/resize workload on
 * several threads and checks that every block keeps its contents and alignment.
 *
 *   cc -O1 -g -fsanitize=undefined -pthread heap_test.c -o heap_test && ./heap_test
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define malloc t_malloc
#define free t_free
#define calloc t_calloc
#define realloc t_realloc
#define memalign t_memalign
#define posix_memalign t_posix_memalign
#define aligned_alloc t_aligned_alloc
#define malloc_usable_size t_malloc_usable_size
#define strdup t_strdup
#define strndup t_strndup
#include "heap.c"
#undef malloc
#undef free
#undef calloc
#undef realloc
#undef memalign
#undef posix_memalign
#undef aligned_alloc
#undef malloc_usable_size
#undef strdup
#undef strndup

/* Stand-ins: "physical" addresses are just a counter; mappings are anonymous memory. */
static _Atomic int64_t next_physical = 0x100000;
static _Atomic long mappings;

int64_t sceKernelGetDirectMemorySize(void)
{
    return (int64_t)12 << 30;
}

int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t length, size_t alignment,
                                  int type, int64_t *physical)
{
    (void)start;
    (void)end;
    (void)alignment;
    (void)type;
    *physical = atomic_fetch_add(&next_physical, (int64_t)length);
    return 0;
}

int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical, size_t alignment)
{
    (void)flags;
    (void)physical;
    (void)alignment;
    void *memory = mmap(NULL, length, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return -1;
    atomic_fetch_add(&mappings, 1);
    *address = memory;
    return 0;
}

int sceKernelReleaseDirectMemory(int64_t physical, size_t length)
{
    (void)physical;
    (void)length;
    atomic_fetch_sub(&mappings, 1);
    return 0;
}

int sceKernelUsleep(unsigned int microseconds)
{
    return usleep(microseconds);
}

#define SLOTS 400

struct slot
{
    unsigned char *pointer;
    size_t size;
    unsigned char seed;
};

static void fill(struct slot *slot)
{
    for (size_t index = 0; index < slot->size; index += 257)
        slot->pointer[index] = (unsigned char)(slot->seed + index);
    if (slot->size > 0)
        slot->pointer[slot->size - 1] = slot->seed;
}

static void check(const struct slot *slot, size_t size, const char *where)
{
    for (size_t index = 0; index < size; index += 257)
        if (slot->pointer[index] != (unsigned char)(slot->seed + index))
        {
            fprintf(stderr, "corrupt block (%s) at offset %zu of %zu\n", where, index, size);
            abort();
        }
}

static size_t random_size(unsigned *state)
{
    unsigned pick = rand_r(state) % 100;
    if (pick < 70)
        return (size_t)rand_r(state) % 2048;
    if (pick < 93)
        return (size_t)rand_r(state) % (512 * 1024);
    if (pick < 99)
        return (size_t)rand_r(state) % (6u << 20);
    return ((size_t)8 << 20) + (size_t)rand_r(state) % (24u << 20); /* large path */
}

static void *worker(void *argument)
{
    unsigned state = (unsigned)(uintptr_t)argument;
    static _Thread_local struct slot slots[SLOTS];
    for (int round = 0; round < 60000; ++round)
    {
        struct slot *slot = &slots[rand_r(&state) % SLOTS];
        unsigned action = rand_r(&state) % 10;
        if (slot->pointer == NULL)
        {
            slot->size = random_size(&state);
            slot->seed = (unsigned char)rand_r(&state);
            if (action < 3)
            {
                size_t alignment = (size_t)32 << (rand_r(&state) % 8);
                slot->pointer = t_memalign(alignment, slot->size);
                if (((uintptr_t)slot->pointer & (alignment - 1)) != 0)
                {
                    fprintf(stderr, "misaligned block\n");
                    abort();
                }
            }
            else
                slot->pointer = t_malloc(slot->size);
            if (slot->pointer == NULL || ((uintptr_t)slot->pointer & 15) != 0)
            {
                fprintf(stderr, "allocation failed or misaligned\n");
                abort();
            }
            if (t_malloc_usable_size(slot->pointer) < slot->size)
            {
                fprintf(stderr, "usable size below request\n");
                abort();
            }
            fill(slot);
        }
        else if (action < 3)
        {
            size_t size = random_size(&state);
            size_t kept = size < slot->size ? size : slot->size;
            if (kept > 0)
                slot->pointer[slot->size - 1] = (unsigned char)(slot->seed + slot->size - 1);
            unsigned char *moved = t_realloc(slot->pointer, size);
            if (size == 0)
            {
                slot->pointer = NULL;
                continue;
            }
            if (moved == NULL)
            {
                fprintf(stderr, "realloc failed\n");
                abort();
            }
            slot->pointer = moved;
            /* The sampled bytes below `kept` must have survived the move. */
            for (size_t index = 0; index + 1 < kept; index += 257)
                if (moved[index] != (unsigned char)(slot->seed + index))
                {
                    fprintf(stderr, "realloc lost contents\n");
                    abort();
                }
            slot->size = size;
            fill(slot);
        }
        else
        {
            if (slot->size > 0)
                slot->pointer[slot->size - 1] = (unsigned char)(slot->seed + slot->size - 1);
            check(slot, slot->size, "free");
            t_free(slot->pointer);
            slot->pointer = NULL;
        }
    }
    for (int index = 0; index < SLOTS; ++index)
        t_free(slots[index].pointer);
    return NULL;
}

int main(void)
{
    /* A pointer the heap does not own must be ignored, not corrupted. */
    static char foreign[64];
    t_free(foreign + 32);
    if (t_realloc(foreign + 32, 10) != NULL || t_malloc_usable_size(foreign + 32) != 0)
    {
        fprintf(stderr, "foreign pointer was treated as owned\n");
        return 1;
    }

    pthread_t threads[8];
    for (uintptr_t index = 0; index < 8; ++index)
        pthread_create(&threads[index], NULL, worker, (void *)(index + 1));
    for (int index = 0; index < 8; ++index)
        pthread_join(threads[index], NULL);

    size_t in_use, peak, mapped;
    app_heap_stats(&in_use, &peak, &mapped);
    printf("in use %zu, peak %zu MiB, mapped %zu MiB, regions %u, large mappings left %ld\n",
           in_use, peak >> 20, mapped >> 20, region_count, (long)mappings - (long)region_count);
    if (in_use != 0 || (long)mappings != (long)region_count)
    {
        fprintf(stderr, "leak: memory still in use after every block was freed\n");
        return 1;
    }
    /* After everything is freed each region must be one free block again. */
    for (unsigned index = 0; index < region_count; ++index)
    {
        struct header *block = (struct header *)regions[index].start;
        if ((block->size_flags & FLAG_USED) || block_size(block) != REGION_BYTES - sizeof *block)
        {
            fprintf(stderr, "region %u did not coalesce\n", index);
            return 1;
        }
    }
    puts("heap test passed");
    return 0;
}
