/*
 * Host check of the per-process allocation budget (scripts/tests/process-limit.sh): the real
 * kernel/task/process.c, task.c, elfloader.c and isr80h/process.c on the stand-ins of
 * process-host.c and the heap of kalloc.c. The script builds it twice: with the shipped config
 * and with a copy whose PROCESS_HEAP_LIMIT_BYTES is 64 KiB (16 pages), where the budget runs out
 * before the 1024 allocation slots do.
 *
 * Every allocation is charged in whole pages, the page-rounded size of the request; the process's
 * heap_used_bytes must always equal the sum of the charges of its live allocations. Cases (each
 * in its own process; L = PROCESS_HEAP_LIMIT_BYTES, P = 4096):
 *   limit     L bytes fit exactly, one more byte is refused; freeing gives the whole charge back
 *             and L fits again
 *   over      L + 1 is refused; L - P + 1 (rounded: L) fits
 *   rounding  1, P and P + 1 bytes cost 1, 1 and 2 pages; the rest of the budget fits, then
 *             nothing does; freeing a 1-byte allocation gives a whole page back
 *   pages     1-byte allocations until one is refused: L / P of them, or 1024 (every slot) if
 *             that is fewer; a refusal for want of a slot charges nothing and allocates nothing
 *   sizes     0, sizes whose page rounding wraps 32 bits and sizes above L are refused, also with
 *             part of the budget used (where used + charge wraps 32 bits)
 *   kzalloc   the heap refuses the allocation: nothing charged, no slot taken
 *   map       mapping the allocation fails: nothing charged, no slot taken, the memory freed
 *   free      revoking the user mapping fails: the allocation and its charge stay; a second free
 *             then gives the charge back
 *   invalid   free of NULL, of an address inside an allocation, of kernel memory the process
 *             doesn't own, of another process's allocation, and a second free of the same
 *             pointer change nothing
 *   corrupt   with heap_used_bytes at L, less than a page short of L, or above L (L + 1, L + P,
 *             2 GiB, 4 GiB - P, 4 GiB - 1), nothing is admitted: the budget does not reopen
 *   underflow freeing an allocation whose charge is more than heap_used_bytes stops the kernel
 *             (PANIC) before heap_used_bytes wraps
 *   argv      the argument array and every argument string are charged; a launch charges the
 *             child, not the caller; a budget too small for them, a heap that refuses any of
 *             them, or a failed mapping of any of them leaves the usage as it was
 *   exit      a child with arguments and allocations ends without a PANIC
 * Prints "process-limit host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "string/string.h"
#include "memory/paging/paging.h"
#include "fs/file.h"
#include "task/task.h"
#include "task/process.h"

#define L ((uint32_t)PROCESS_HEAP_LIMIT_BYTES)
// The usage as the 32-bit kernel sees it (heap_used_bytes may be wider on the host).
#define USED(p) ((uint32_t)(p)->heap_used_bytes)
#define P ((uint32_t)PAGING_PAGE_SIZE)

extern struct task *current_task;

static unsigned char blank[65536];
static int blank_size;
static struct process *caller;
static struct command_argument args[4];
static int failed;
// Set by the underflow case: the PANIC it expects, and the usage that must not have wrapped.
static struct process *panic_process;
static uint32_t panic_used;

#define FAIL(...) do { printf("  FAIL %s: ", current_case); printf(__VA_ARGS__); printf("\n"); failed = 1; } while (0)
static const char *current_case = "?";

void __real_PANIC(const char *msg, const char *file, int line);
void __wrap_PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s\n", msg);
    if (!panic_process)
    {
        printf("  FAIL %s: unexpected PANIC\n", current_case);
        fflush(0);
        _exit(1);
    }
    if (USED(panic_process) != panic_used)
    {
        printf("  FAIL %s: heap_used_bytes changed to %u before the PANIC (was %u)\n", current_case,
               USED(panic_process), panic_used);
        fflush(0);
        _exit(1);
    }
    fflush(0);
    _exit(failed ? 1 : 3);
}

static uint32_t charge(uint32_t size)
{
    return (size + P - 1) / P * P;
}

static int slots_used(struct process *p)
{
    int n = 0;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
    {
        n += p->allocations[i].ptr != 0;
    }
    return n;
}

static struct process_allocation *slot_of(struct process *p, void *ptr)
{
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
    {
        if (ptr && p->allocations[i].ptr == ptr)
        {
            return &p->allocations[i];
        }
    }
    return 0;
}

// heap_used_bytes is the sum of the page-rounded charges of the live allocations.
static void ledger(struct process *p, const char *when)
{
    uint64_t sum = 0;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
    {
        if (p->allocations[i].ptr)
        {
            sum += charge(p->allocations[i].size);
        }
    }
    if (sum != USED(p))
    {
        FAIL("%s: heap_used_bytes is %u, the live allocations are charged %llu", when, USED(p),
             (unsigned long long)sum);
    }
    if (host_paging_problems || kalloc_bad_frees() || kalloc_poison_errors())
    {
        FAIL("%s: paging problem (%s), %d bad frees, %d writes after kfree", when,
             host_paging_problems ? host_paging_problem : "none", kalloc_bad_frees(), kalloc_poison_errors());
    }
}

// The caller in slot 0, running, with no arguments and nothing charged.
static void start(void)
{
    kalloc_init();
    kalloc_set_alignment(PAGING_PAGE_SIZE);
    host_reset();
    host_file_serve(blank, blank_size);
    caller = host_load_caller();
    host_reset();
    if (USED(caller) != 0 || slots_used(caller) != 0)
    {
        printf("the caller starts with %u bytes charged in %d slots\n", USED(caller), slots_used(caller));
        exit(2);
    }
}

// size must fit: it is charged in whole pages and kept at its exact size.
static void *admit(struct process *p, uint32_t size, const char *what)
{
    uint32_t before = USED(p);
    void *ptr = process_malloc(p, size);
    struct process_allocation *slot = slot_of(p, ptr);
    if (!ptr || !slot)
    {
        FAIL("%s: %u bytes refused with %u of %u used", what, size, before, L);
        return 0;
    }
    if (slot->size != size)
    {
        FAIL("%s: %u bytes kept as %u", what, size, (uint32_t)slot->size);
    }
    if (USED(p) != before + charge(size))
    {
        FAIL("%s: %u bytes charged %u, expected %u", what, size, USED(p) - before, charge(size));
    }
    return ptr;
}

// size must be refused before anything is allocated, mapped or charged.
static void refuse(struct process *p, uint32_t size, const char *what)
{
    uint32_t before = USED(p);
    int slots = slots_used(p), calls = kalloc_calls(), maps = host_map_calls, live = kalloc_live();
    void *ptr = process_malloc(p, size);
    if (ptr)
    {
        FAIL("%s: %u bytes admitted with %u of %u used", what, size, before, L);
        return;
    }
    if (USED(p) != before || slots_used(p) != slots)
    {
        FAIL("%s: refusing %u bytes changed the usage %u -> %u or the slots %d -> %d", what, size, before,
             USED(p), slots, slots_used(p));
    }
    if (kalloc_calls() != calls || host_map_calls != maps || kalloc_live() != live)
    {
        FAIL("%s: refusing %u bytes allocated or mapped first (%d allocation calls, %d mappings)", what, size,
             kalloc_calls() - calls, host_map_calls - maps);
    }
}

// A free that must change nothing.
static void free_nothing(struct process *p, void *ptr, const char *what)
{
    uint32_t before = USED(p);
    int slots = slots_used(p), live = kalloc_live();
    process_free(p, ptr);
    if (USED(p) != before || slots_used(p) != slots || kalloc_live() != live)
    {
        FAIL("%s: usage %u -> %u, slots %d -> %d, live allocations %d -> %d", what, before, USED(p),
             slots, slots_used(p), live, kalloc_live());
    }
}

// A free that must give back exactly the charge of ptr.
static void free_all(struct process *p, void *ptr, const char *what)
{
    struct process_allocation *slot = slot_of(p, ptr);
    uint32_t before = USED(p), size = slot ? slot->size : 0;
    process_free(p, ptr);
    if (!slot || slot_of(p, ptr) || kalloc_is_live(ptr))
    {
        FAIL("%s: the allocation was not freed", what);
    }
    else if (USED(p) != before - charge(size))
    {
        FAIL("%s: freeing %u bytes gave back %u, expected %u", what, size, before - USED(p), charge(size));
    }
}

static void copy_text(char *to, const char *from)
{
    while ((*to++ = *from++))
        ;
}

static struct command_argument *arguments(int count)
{
    static const char *const words[] = {"child", "one", "two", "three"};
    for (int i = 0; i < count; i++)
    {
        copy_text(args[i].argument, words[i]);
        args[i].next = i + 1 < count ? &args[i + 1] : 0;
    }
    return &args[0];
}

// The charge of an argument list of count words: the pointer array and one copy per word.
static uint32_t argv_charge(int count)
{
    return charge(sizeof(char *) * count) + count * charge(sizeof(args[0].argument));
}

static void limit_case(int unused1, int unused2)
{
    start();
    void *a = admit(caller, L, "the whole budget");
    refuse(caller, 1, "one byte over the budget");
    ledger(caller, "full");
    free_all(caller, a, "free the whole budget");
    a = admit(caller, L, "the whole budget again");
    refuse(caller, P, "a page over the budget again");
    ledger(caller, "full again");
}

static void over_case(int unused1, int unused2)
{
    start();
    refuse(caller, L + 1, "one byte more than the budget");
    refuse(caller, L + P, "a page more than the budget");
    admit(caller, L - P + 1, "a size that rounds up to the budget");
    refuse(caller, 1, "one byte after the rounded budget");
    ledger(caller, "full");
}

static void rounding_case(int unused1, int unused2)
{
    start();
    void *one = admit(caller, 1, "1 byte");
    admit(caller, P, "one page");
    admit(caller, P + 1, "one page and a byte");
    if (USED(caller) != 4 * P)
    {
        FAIL("1, %u and %u bytes are charged %u, expected %u", P, P + 1, USED(caller), 4 * P);
    }
    if (L > 4 * P)
    {
        refuse(caller, L - 4 * P + 1, "a byte more than the rest");
        admit(caller, L - 4 * P, "the rest of the budget");
    }
    refuse(caller, 1, "one byte after the budget");
    free_all(caller, one, "free the 1-byte allocation");
    admit(caller, P, "a page in its place");
    refuse(caller, 1, "one byte after that");
    ledger(caller, "full");
}

static void pages_case(int unused1, int unused2)
{
    start();
    int expected = L / P < RORTHOX_MAX_PROGRAM_ALLOCATIONS ? (int)(L / P) : RORTHOX_MAX_PROGRAM_ALLOCATIONS;
    void *first = 0;
    int n = 0;
    while (n <= RORTHOX_MAX_PROGRAM_ALLOCATIONS)
    {
        void *ptr = process_malloc(caller, 1);
        if (!ptr)
        {
            break;
        }
        first = first ? first : ptr;
        n++;
    }
    if (n != expected)
    {
        FAIL("%d 1-byte allocations fit, expected %d", n, expected);
    }
    if (USED(caller) != (uint32_t)n * P)
    {
        FAIL("%d 1-byte allocations are charged %u, expected %u", n, USED(caller), n * P);
    }
    // Whatever stopped it (budget or slots), the next request is refused before anything happens.
    refuse(caller, 1, "one more byte");
    refuse(caller, P, "one more page");
    ledger(caller, "after the last allocation");
    free_all(caller, first, "free one");
    admit(caller, 1, "one byte in its place");
    ledger(caller, "after the reuse");
}

static void sizes_case(int unused1, int unused2)
{
    start();
    refuse(caller, 0, "0 bytes");
    refuse(caller, UINT32_MAX, "4 GiB - 1");
    refuse(caller, UINT32_MAX - P + 2, "a size whose rounding wraps");
    refuse(caller, UINT32_MAX - P + 1, "the largest page multiple");
    refuse(caller, 0x80000000u, "2 GiB");
    admit(caller, 2 * P, "two pages");
    // used + charge wraps 32 bits to 0 or a small value
    refuse(caller, 0u - 2 * P, "2 pages short of 4 GiB with 2 pages used");
    refuse(caller, 0u - 2 * P - 1, "a size that rounds to 2 pages short of 4 GiB");
    refuse(caller, 0u - P, "1 page short of 4 GiB with 2 pages used");
    refuse(caller, 0, "0 bytes with 2 pages used");
    ledger(caller, "after the refusals");
}

static void kzalloc_case(int unused1, int unused2)
{
    start();
    uint32_t before = USED(caller);
    int live = kalloc_live();
    kalloc_fail_at(kalloc_calls() + 1);
    void *ptr = process_malloc(caller, P);
    if (ptr || !kalloc_failed())
    {
        FAIL("the heap refused (%d), process_malloc returned %p", kalloc_failed(), ptr);
    }
    if (USED(caller) != before || slots_used(caller) || kalloc_live() != live)
    {
        FAIL("usage %u, %d slots, %d live allocations after the heap refused", USED(caller),
             slots_used(caller), kalloc_live() - live);
    }
    admit(caller, L, "the whole budget afterwards");
    ledger(caller, "after the heap refused");
}

static void map_case(int unused1, int unused2)
{
    start();
    int live = kalloc_live();
    host_fail_map_at = host_map_calls + 1;
    void *ptr = process_malloc(caller, 3 * P);
    if (ptr || !host_failed)
    {
        FAIL("the mapping failed (%d), process_malloc returned %p", host_failed, ptr);
    }
    if (USED(caller) || slots_used(caller) || kalloc_live() != live)
    {
        FAIL("usage %u, %d slots, %d live allocations after the mapping failed", USED(caller),
             slots_used(caller), kalloc_live() - live);
    }
    admit(caller, L, "the whole budget afterwards");
    ledger(caller, "after the failed mapping");
}

static void free_case(int unused1, int unused2)
{
    start();
    void *a = admit(caller, P + 1, "two pages");
    void *b = admit(caller, 1, "one page");
    host_fail_unmap_at = host_unmap_calls + 1;
    free_nothing(caller, a, "free whose revocation fails");
    if (!host_unmap_failed || !slot_of(caller, a))
    {
        FAIL("the revocation did not fail (%d) or the allocation is gone", host_unmap_failed);
    }
    ledger(caller, "after the failed free");
    free_all(caller, a, "the same free again");
    free_all(caller, b, "free the other one");
    if (USED(caller))
    {
        FAIL("%u bytes still charged with nothing allocated", USED(caller));
    }
    admit(caller, L, "the whole budget afterwards");
}

static void invalid_case(int unused1, int unused2)
{
    start();
    struct process *child = 0;
    host_file_serve(blank, blank_size);
    if (process_load_for_slot("0:/child", &child, 1) < 0 || !child)
    {
        printf("could not load a second process\n");
        exit(2);
    }
    void *theirs = admit(child, P, "the other process's page");
    char *a = admit(caller, 2 * P, "two pages");
    void *b = admit(caller, 1, "one page");
    void *kernel = kzalloc(P);
    free_nothing(caller, 0, "free(NULL)");
    free_nothing(caller, a + 1, "free inside an allocation");
    free_nothing(caller, a + P, "free of an allocation's second page");
    free_nothing(caller, kernel, "free of kernel memory");
    free_nothing(caller, theirs, "free of another process's allocation");
    if (USED(child) != P || !slot_of(child, theirs))
    {
        FAIL("the other process's usage is %u", USED(child));
    }
    free_all(caller, a, "free the two pages");
    free_nothing(caller, a, "free them again");
    free_all(caller, b, "free the page");
    free_nothing(caller, b, "free it again");
    if (USED(caller))
    {
        FAIL("%u bytes still charged with nothing allocated", USED(caller));
    }
    admit(caller, L, "the whole budget afterwards");
    refuse(caller, 1, "one byte over it");
    ledger(caller, "after the invalid frees");
    ledger(child, "the other process");
}

// heap_used_bytes set to value (above the budget, or with less than a page left): nothing fits.
static void corrupt_case(int bits, int unused)
{
    uint32_t value = (uint32_t)bits;
    start();
    caller->heap_used_bytes = value;
    refuse(caller, 1, "1 byte");
    refuse(caller, P, "a page");
    refuse(caller, 0x80000000u, "2 GiB");
    refuse(caller, 0u - value, "the size that wraps the sum to 0");
    if (USED(caller) != value)
    {
        FAIL("heap_used_bytes changed from %u to %u", value, USED(caller));
    }
}

// Ends with the PANIC (exit 3) if right.
static void underflow_case(int used, int unused)
{
    start();
    void *a = admit(caller, 1, "one page");
    caller->heap_used_bytes = used;
    panic_process = caller;
    panic_used = used;
    process_free(caller, a);
    FAIL("freeing a page with %d bytes charged did not stop the kernel (heap_used_bytes is now %u)", used,
         USED(caller));
}

static void argv_case(int unused1, int unused2)
{
    start();
    if (process_inject_arguments(caller, arguments(3)) != 0)
    {
        FAIL("3 arguments were not injected");
    }
    if (USED(caller) != argv_charge(3))
    {
        FAIL("3 arguments are charged %u, expected %u", USED(caller), argv_charge(3));
    }
    ledger(caller, "after injecting 3 arguments");

    // A launch charges the child.
    struct process *child = 0;
    host_file_serve(blank, blank_size);
    uint32_t mine = USED(caller);
    if (process_load_with_arguments("0:/child", &child, arguments(2)) < 0 || !child)
    {
        FAIL("the child with 2 arguments did not load");
        return;
    }
    if (USED(child) != argv_charge(2) || USED(caller) != mine)
    {
        FAIL("the launch charged the child %u (expected %u) and the caller %u (was %u)", USED(child),
             argv_charge(2), USED(caller), mine);
    }
    ledger(child, "the child");
}

// The budget has room for all but the last page of 3 arguments.
static void argv_budget_case(int unused1, int unused2)
{
    start();
    if (L < argv_charge(3))
    {
        return;
    }
    void *a = L > argv_charge(3) ? admit(caller, L - argv_charge(3) + P, "all but the arguments less a page") : 0;
    uint32_t before = USED(caller);
    int live = kalloc_live();
    if (process_inject_arguments(caller, arguments(3)) >= 0)
    {
        FAIL("3 arguments injected into a budget a page too small");
    }
    if (USED(caller) != before || slots_used(caller) != (a ? 1 : 0) || kalloc_live() != live)
    {
        FAIL("the refused arguments left usage %u (was %u), %d slots, %d live allocations", USED(caller),
             before, slots_used(caller), kalloc_live() - live);
    }
    if (a)
    {
        free_all(caller, a, "free the filler");
        a = admit(caller, L - argv_charge(3), "all but the arguments");
    }
    if (process_inject_arguments(caller, arguments(3)) != 0 || USED(caller) != L)
    {
        FAIL("3 arguments did not fill the budget exactly (usage %u)", USED(caller));
    }
    ledger(caller, "after the arguments");
}

// The n-th allocation (heap) or mapping (map) of 3 arguments fails.
static void argv_fail_case(int map, int n)
{
    start();
    void *a = admit(caller, P, "one page before");
    uint32_t before = USED(caller);
    int live = kalloc_live();
    if (map)
    {
        host_fail_map_at = host_map_calls + n;
    }
    else
    {
        kalloc_fail_at(kalloc_calls() + n);
    }
    int res = process_inject_arguments(caller, arguments(3));
    if (res >= 0 || !(map ? host_failed : kalloc_failed()))
    {
        FAIL("%s %d did not fail the injection (%d)", map ? "mapping" : "allocation", n, res);
    }
    if (USED(caller) != before || slots_used(caller) != 1 || kalloc_live() != live)
    {
        FAIL("after %s %d failed: usage %u (was %u), %d slots, %d live allocations", map ? "mapping" : "allocation", n,
             USED(caller), before, slots_used(caller), kalloc_live() - live);
    }
    ledger(caller, "after the failed injection");
    free_all(caller, a, "free the page");
    if (USED(caller))
    {
        FAIL("%u bytes still charged with nothing allocated", USED(caller));
    }
}

static void exit_case(int unused1, int unused2)
{
    start();
    struct process *child = 0;
    host_file_serve(blank, blank_size);
    if (process_load_with_arguments("0:/child", &child, arguments(3)) < 0 || !child)
    {
        FAIL("the child did not load");
        return;
    }
    admit(child, 1, "a child page");
    admit(child, 3 * P, "three child pages");
    ledger(child, "the child before it ends");
    if (process_terminate(child) != 0)
    {
        FAIL("the child did not end");
    }
    if (host_paging_problems || kalloc_bad_frees() || kalloc_poison_errors())
    {
        FAIL("ending the child: paging problem (%s), bad frees or writes after kfree",
             host_paging_problems ? host_paging_problem : "none");
    }
}

static int cases;
static int case_failures;

// Runs one case, fn(a, b), in its own process. expect_panic: the case must end in the expected PANIC.
static void run(const char *name, void (*fn)(int, int), int a, int b, int expect_panic)
{
    cases++;
    fflush(0);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        current_case = name;
        fn(a, b);
        fflush(0);
        _exit(failed ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    int ok = expect_panic ? code == 3 : code == 0;
    if (!ok)
    {
        if (expect_panic && code == 0)
        {
            printf("  FAIL %s: no PANIC\n", name);
        }
        printf("  case %s: FAILED (exit %d)\n", name, code);
        case_failures++;
    }
    else
    {
        printf("  case %s: ok\n", name);
    }
}

int main(int argc, char **argv)
{
    int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size <= 0)
    {
        printf("usage: process-limit blank.elf\n");
        return 2;
    }
    close(fd);
    printf("budget %u bytes (%u pages), %d allocation slots\n", L, L / P, RORTHOX_MAX_PROGRAM_ALLOCATIONS);

    run("limit", limit_case, 0, 0, 0);
    run("over", over_case, 0, 0, 0);
    run("rounding", rounding_case, 0, 0, 0);
    run("pages", pages_case, 0, 0, 0);
    run("sizes", sizes_case, 0, 0, 0);
    run("kzalloc", kzalloc_case, 0, 0, 0);
    run("map", map_case, 0, 0, 0);
    run("free", free_case, 0, 0, 0);
    run("invalid", invalid_case, 0, 0, 0);
    static const uint32_t corrupt[] = {L, L - P + 1, L + 1, L + P, 0x80000000u, UINT32_MAX - P + 1, UINT32_MAX};
    static char corrupt_names[7][48];
    for (int i = 0; i < 7; i++)
    {
        host_snprintf(corrupt_names[i], sizeof(corrupt_names[0]), "corrupt (used %u)", corrupt[i]);
        run(corrupt_names[i], corrupt_case, (int)corrupt[i], 0, 0);
    }
    run("underflow (none charged)", underflow_case, 0, 0, 1);
    run("underflow (a byte short)", underflow_case, P - 1, 0, 1);
    run("argv", argv_case, 0, 0, 0);
    run("argv budget", argv_budget_case, 0, 0, 0);
    static char names[8][40];
    for (int n = 1; n <= 4; n++)
    {
        host_snprintf(names[2 * n - 2], sizeof(names[0]), "argv allocation %d fails", n);
        run(names[2 * n - 2], argv_fail_case, 0, n, 0);
        host_snprintf(names[2 * n - 1], sizeof(names[0]), "argv mapping %d fails", n);
        run(names[2 * n - 1], argv_fail_case, 1, n, 0);
    }
    run("exit", exit_case, 0, 0, 0);

    printf("process-limit host check: %d of %d cases failed\n", case_failures, cases);
    return case_failures ? 1 : 0;
}
