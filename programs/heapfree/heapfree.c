/*
 * Heap free test for scripts/tests/heap-free.sh (only copied into the test image).
 *
 * The kernel's own kernel/memory/heap/heap.c is built into this program and run on a small
 * private heap: four blocks whose start address is the real heap address (it is only used as a
 * number, nothing at that address is touched). The block table lives in the middle of an array
 * filled with guard bytes, so a free that writes outside the table changes a guard byte instead
 * of memory the program doesn't own. The part before the table is longer than heap address /
 * block size, so even free(NULL) on a heap with no checks lands in it.
 *
 * Every case checks the whole array (guards and table) against what it should be afterwards.
 * The program also calls the real free(NULL) syscall once.
 *
 * All state is in one malloc()ed struct: the loader doesn't map .bss, so no globals.
 *
 * Usage: heapfree.elf <name>. Every run ends with "heapfree <name>: done"; the result is
 * "heapfree <name>: PASS" or one "heapfree: FAIL ..." line per failing case.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "memory.h"
#include "config.h"
#include "memory/heap/heap.h"

#define BLOCK RORTHOX_HEAP_BLOCK_SIZE
#define BLOCKS 4
// More bytes before the table than the block index of NULL on an unchecked heap.
#define PREFIX (2 * (RORTHOX_HEAP_ADDRESS / BLOCK))
#define SUFFIX 64
#define TOTAL (PREFIX + BLOCKS + SUFFIX)
// Guard bytes look like a taken one-block run (TAKEN | IS_FIRST), so only the bounds checks keep
// heap_free() off them. HEAP_BLOCK_HAS_NEXT is clear, so an unchecked free writes one byte and stops.
#define GUARD (HEAP_BLOCK_TABLE_ENTRY_TAKEN | HEAP_BLOCK_IS_FIRST)
#define BASE ((char *)RORTHOX_HEAP_ADDRESS)

_Static_assert(PREFIX > RORTHOX_HEAP_ADDRESS / BLOCK, "prefix must cover the block index of NULL");
_Static_assert((GUARD & HEAP_BLOCK_HAS_NEXT) == 0, "guard byte must not look like a chain");

#define FIRST_TAKEN (HEAP_BLOCK_TABLE_ENTRY_TAKEN | HEAP_BLOCK_IS_FIRST)

_Static_assert((GUARD & HEAP_BLOCK_TABLE_ENTRY_TAKEN) && (GUARD & HEAP_BLOCK_IS_FIRST), "guard byte must look like a run start");

struct test
{
    unsigned char memory[TOTAL];
    unsigned char expected[TOTAL];
    struct heap_table table;
    struct heap heap;
    int failed;
};

// One print call per line: the shell runs alongside and may print between two calls.
static void say(const char *name, const char *text)
{
    char line[80];
    strcpy(line, "heapfree ");
    strncpy(line + strlen(line), name, 16);
    strcpy(line + strlen(line), ": ");
    strcpy(line + strlen(line), text);
    strcpy(line + strlen(line), "\n");
    print(line);
}

static void fail(struct test *t, const char *line)
{
    print(line);
    t->failed = 1;
}

static HEAP_BLOCK_TABLE_ENTRY *entries(struct test *t)
{
    return t->memory + PREFIX;
}

// Guards everywhere, then an empty four-block heap.
static int reset(struct test *t)
{
    memset(t->memory, GUARD, TOTAL);
    t->table.entries = entries(t);
    t->table.total = BLOCKS;
    return heap_create(&t->heap, BASE, BASE + BLOCKS * BLOCK, &t->table);
}

static void snapshot(struct test *t)
{
    memcpy(t->expected, t->memory, TOTAL);
}

static int unchanged(struct test *t)
{
    return memcmp(t->expected, t->memory, TOTAL) == 0;
}

// a = block 0 (one block), b = blocks 1-3 (three blocks).
static int setup_two(struct test *t, char **a, char **b)
{
    if (reset(t) < 0)
    {
        return 0;
    }
    *a = heap_malloc(&t->heap, BLOCK);
    *b = heap_malloc(&t->heap, 3 * BLOCK);
    return *a == BASE && *b == BASE + BLOCK && entries(t)[0] == FIRST_TAKEN;
}

static void case_null(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for NULL free\n");
        return;
    }
    snapshot(t);
    heap_free(&t->heap, NULL);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL NULL free changed guard\n");
    }
}

static void case_below(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for free below heap\n");
        return;
    }
    snapshot(t);
    heap_free(&t->heap, BASE - BLOCK);
    heap_free(&t->heap, BASE - 1);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL free below heap changed memory\n");
    }
}

static void case_end(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for free past heap end\n");
        return;
    }
    snapshot(t);
    heap_free(&t->heap, BASE + BLOCKS * BLOCK);
    heap_free(&t->heap, BASE + (BLOCKS + 1) * BLOCK);
    heap_free(&t->heap, (void *)0xFFFFF000);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL free past heap end changed memory\n");
    }
}

static void case_unaligned(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for unaligned free\n");
        return;
    }
    snapshot(t);
    heap_free(&t->heap, a + 1);
    heap_free(&t->heap, b + 16);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL unaligned free changed table\n");
    }
}

static void case_middle(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for middle block free\n");
        return;
    }
    snapshot(t);
    heap_free(&t->heap, b + BLOCK);
    heap_free(&t->heap, b + 2 * BLOCK);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL middle block free changed table\n");
    }
}

// A valid free of b, then the same free again.
static void case_valid_and_double(struct test *t)
{
    char *a, *b;
    if (!setup_two(t, &a, &b))
    {
        fail(t, "heapfree: FAIL setup for valid free\n");
        return;
    }
    snapshot(t);
    t->expected[PREFIX + 1] = HEAP_BLOCK_TABLE_ENTRY_FREE;
    t->expected[PREFIX + 2] = HEAP_BLOCK_TABLE_ENTRY_FREE;
    t->expected[PREFIX + 3] = HEAP_BLOCK_TABLE_ENTRY_FREE;
    heap_free(&t->heap, b);
    if (!unchanged(t) || entries(t)[0] != FIRST_TAKEN)
    {
        fail(t, "heapfree: FAIL valid free wrong result\n");
        return;
    }

    snapshot(t);
    heap_free(&t->heap, b);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL double free changed table\n");
    }
}

static void case_last_block_and_exact_fit(struct test *t)
{
    if (reset(t) < 0)
    {
        fail(t, "heapfree: FAIL setup for last block\n");
        return;
    }
    snapshot(t);
    char *c = heap_malloc(&t->heap, 3 * BLOCK);
    char *d = heap_malloc(&t->heap, 1);
    if (c != BASE || d != BASE + 3 * BLOCK || entries(t)[3] != FIRST_TAKEN)
    {
        fail(t, "heapfree: FAIL last block allocation\n");
        return;
    }
    heap_free(&t->heap, d);
    heap_free(&t->heap, c);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL last block free\n");
        return;
    }

    char *all = heap_malloc(&t->heap, BLOCKS * BLOCK);
    void *more = heap_malloc(&t->heap, 1);
    if (all != BASE || more != NULL || entries(t)[0] != (FIRST_TAKEN | HEAP_BLOCK_HAS_NEXT) ||
        entries(t)[3] != HEAP_BLOCK_TABLE_ENTRY_TAKEN)
    {
        fail(t, "heapfree: FAIL exact fit allocation\n");
        return;
    }
    heap_free(&t->heap, all);
    if (!unchanged(t))
    {
        fail(t, "heapfree: FAIL exact fit free\n");
    }
}

static void case_sizes(struct test *t)
{
    if (reset(t) < 0)
    {
        fail(t, "heapfree: FAIL setup for size requests\n");
        return;
    }
    snapshot(t);
    void *zero = heap_malloc(&t->heap, 0);
    void *wrap_low = heap_malloc(&t->heap, 0xFFFFF001U);
    void *wrap_high = heap_malloc(&t->heap, 0xFFFFFFFFU);
    void *too_big = heap_malloc(&t->heap, BLOCKS * BLOCK + 1);
    if (zero || wrap_low || wrap_high || too_big || !unchanged(t))
    {
        fail(t, "heapfree: FAIL zero or huge size request\n");
    }
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "?";

    // The real syscall: the kernel must ignore it and keep this process running.
    free(NULL);
    say(name, "free(NULL) returned");

    struct test *t = malloc(sizeof(struct test));
    if (!t)
    {
        print("heapfree: FAIL setup allocation\n");
        say(name, "done");
        return 1;
    }
    t->failed = 0;

    case_null(t);
    case_below(t);
    case_end(t);
    case_unaligned(t);
    case_middle(t);
    case_valid_and_double(t);
    case_last_block_and_exact_fit(t);
    case_sizes(t);

    int failed = t->failed;
    free(t);
    if (!failed)
    {
        say(name, "PASS");
    }
    say(name, "done");
    return failed;
}
