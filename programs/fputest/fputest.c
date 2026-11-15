/*
 * x87 and SSE state test for scripts/tests/fpu-state.sh (only copied into the test images).
 *
 * Every program has its own x87 and SSE registers: the kernel saves them when it switches away
 * from a program and puts them back when the program runs again, and a new program starts with
 * clean ones (control word 0x037F, empty stack, zero registers, MXCSR 0x1F80). The registers are
 * loaded and checked with explicit instructions; nothing in C uses floating point. Only SSE
 * (Pentium III) instructions are used, never SSE2.
 *
 * A check uses the registers: the sum of the seven x87 values, 1/3 under the control word
 * (rounding and precision), FNSTCW and the stack top, STMXCSR, the XMM registers stored, and
 * divps xmm0 / xmm1 under the MXCSR rounding, against the results right after loading.
 *
 * "fputest.elf" (A): loads its x87 state, starts "fputest.elf fresh x87", sleeps and checks.
 *     Then it starts "fputest.elf worker" (B), and both run:
 *       x87 windows   load the x87 state, then spin without a syscall until the kernel has come
 *                     back to the program from an interrupt (GS was set to 0, the return to ring 3
 *                     loads it again), then check; a window that took 2 or more timer ticks was
 *                     switched away from (another task ran while the state was loaded)
 *       sse windows   the same with the x87 and SSE state loaded (A first starts
 *                     "fputest.elf fresh sse" with both loaded, sleeps and checks)
 *       syscall windows  both loaded: a sleep of 20 ms (another task runs), or a syscall that
 *                     returns to the same task, then check
 *     While it spins, EDI holds 0xF0<id>00<87|55> (id 0x41 A, 0x42 B; 0x87 x87, 0x55 sse), so a
 *     QEMU monitor sample with that EDI in ring 3 must show that program's registers.
 * "fputest.elf fresh <tag>": FXSAVE as its first FPU instruction must show the clean
 *     state, then 1/3 on the x87 and with divps must be rounded to nearest. "fputest.elf afterexit"
 *     and "fputest.elf afterfault" do the same (tag = the mode), then wait for a key. They have as
 *     many words as "fputest.elf dirty" and "fputest.elf fault", so the shell's command takes the
 *     same heap blocks and the new task gets the block of the program that ended before it.
 * "fputest.elf dirty" / "fputest.elf fault": loads its state (I), waits for a key, then ends with
 *     exit / a page fault right after loading a last XMM7 (see dirty()).
 * "fputest.elf mf": divides by zero with the zero-divide exception unmasked, then FWAIT: the x87
 *     error (#MF) must end it.
 * As the first program (no arguments, started as shell.elf): loads its state (I) and sleeps in
 *     100 ms steps until a key comes (the kernel idles meanwhile, no other task exists), checks,
 *     starts "shell.elf fresh idle" with its state loaded, sleeps, checks again, then sleeps for ever.
 *
 * Results: "<who>: <failed> of <checks> checks failed" (the first failed check of each is also
 * printed, "<who>: FAIL <check>"). One print call per line: other programs run alongside.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "memory.h"

struct pattern
{
    uint32_t xmm[8][4];
    const char *who;
    uint32_t id;
    uint16_t fcw;
    int32_t base;
    int32_t step;
    uint32_t mxcsr;
};

#define F1 0x3F800000U
#define F2 0x40000000U
#define F3 0x40400000U
#define F4 0x40800000U
#define F5 0x40A00000U
#define F7 0x40E00000U
#define F8 0x41000000U
#define F10 0x41200000U
#define F11 0x41300000U
#define F13 0x41500000U
#define F14 0x41600000U
#define F16 0x41800000U
#define F17 0x41880000U
#define LANES(id, i) {(id) << 24 | (i) << 16, (id) << 24 | (i) << 16 | 1, (id) << 24 | (i) << 16 | 2, (id) << 24 | (i) << 16 | 3}
#define REST(id) LANES(id, 2), LANES(id, 3), LANES(id, 4), LANES(id, 5), LANES(id, 6), LANES(id, 7)

// x87: A rounds down (64-bit precision), B up (53-bit), I toward zero (53-bit). MXCSR: every
// exception masked, the precision flag set, rounding down / up / toward zero.
static const struct pattern pattern_a __attribute__((aligned(16))) =
    {{{F1, F2, F4, F5}, {F3, F3, F3, F3}, REST(0x41U)}, "fputest A", 0x41, 0x077F, 1000, 111, 0x3FA0};
static const struct pattern pattern_b __attribute__((aligned(16))) =
    {{{F7, F8, F10, F11}, {F3, F3, F3, F3}, REST(0x42U)}, "fputest B", 0x42, 0x0A7F, -2000, 37, 0x5FA0};
static const struct pattern pattern_i __attribute__((aligned(16))) =
    {{{F13, F14, F16, F17}, {F3, F3, F3, F3}, REST(0x49U)}, "fputest I", 0x49, 0x0E7F, 30000, 1013, 0x7FA0};

static const int32_t three = 3;
static const int32_t zero_divisor = 0;

#define WINDOWS 150
#define SLEEPS 10
#define RETURNS 50

struct tally
{
    char who[48];
    int checks;
    int failed;
};

static char *add(char *line, const char *text)
{
    strcpy(line + strlen(line), text);
    return line;
}

static char *add_number(char *line, int n)
{
    return add(line, itoa(n));
}

static void say(const char *who, const char *text)
{
    char line[160] = "";
    print(add(add(add(add(line, who), ": "), text), "\n"));
}

static void begin(struct tally *t, const char *who, const char *what)
{
    t->who[0] = 0;
    add(add(add(t->who, who), " "), what);
    t->checks = 0;
    t->failed = 0;
}

static void check(struct tally *t, int ok, const char *text)
{
    t->checks++;
    if (!ok && ++t->failed == 1)
    {
        char line[160] = "";
        print(add(add(add(add(line, t->who), ": FAIL "), text), "\n"));
    }
}

// "<who>: <failed> of <checks> checks failed<more>"
static void summary(struct tally *t, const char *more)
{
    char line[160] = "";
    add(add_number(add(add_number(add(add(line, t->who), ": "), t->failed), " of "), t->checks), " checks failed");
    print(add(add(line, more), "\n"));
}

static int same(const void *a, const void *b, int size)
{
    const unsigned char *x = a, *y = b;
    for (int i = 0; i < size; i++)
    {
        if (x[i] != y[i])
        {
            return 0;
        }
    }
    return 1;
}

// The results right after loading, which every later check must give again.
struct expected
{
    unsigned char third[10];
    uint32_t quotient[4];
    int32_t sum;
};

static void x87_load(const struct pattern *p)
{
    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(p->fcw));
    for (int i = 0; i < 7; i++)
    {
        int32_t value = p->base + i * p->step;
        __asm__ volatile("fildl %0" : : "m"(value));
    }
}

static void sse_load(const struct pattern *p)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(p->mxcsr));
    __asm__ volatile(
        "movups 0(%0), %%xmm0\n\t"
        "movups 16(%0), %%xmm1\n\t"
        "movups 32(%0), %%xmm2\n\t"
        "movups 48(%0), %%xmm3\n\t"
        "movups 64(%0), %%xmm4\n\t"
        "movups 80(%0), %%xmm5\n\t"
        "movups 96(%0), %%xmm6\n\t"
        "movups 112(%0), %%xmm7"
        : : "r"(p->xmm) : "memory");
}

// Uses the one free x87 register: the sum of all seven values, then 1/3, each popped again.
static void x87_compute(int32_t *sum, unsigned char *third)
{
    __asm__ volatile(
        "fld %%st(0)\n\t"
        "fadd %%st(2), %%st\n\t"
        "fadd %%st(3), %%st\n\t"
        "fadd %%st(4), %%st\n\t"
        "fadd %%st(5), %%st\n\t"
        "fadd %%st(6), %%st\n\t"
        "fadd %%st(7), %%st\n\t"
        "fistpl %0"
        : "=m"(*sum));
    __asm__ volatile("fld1\n\tfidivl %1\n\tfstpt %0" : "=m"(*(unsigned char (*)[10])third) : "m"(three));
}

// xmm0 / xmm1 under the current MXCSR; xmm0 is loaded again from the pattern afterwards.
static void sse_compute(const struct pattern *p, uint32_t *quotient)
{
    __asm__ volatile(
        "divps %%xmm1, %%xmm0\n\t"
        "movups %%xmm0, (%0)\n\t"
        "movups (%1), %%xmm0"
        : : "r"(quotient), "r"(p->xmm[0]) : "memory");
}

static void load(const struct pattern *p, struct expected *e, int sse)
{
    x87_load(p);
    x87_compute(&e->sum, e->third);
    if (sse)
    {
        sse_load(p);
        sse_compute(p, e->quotient);
    }
}

static void verify(struct tally *t, const struct pattern *p, const struct expected *e, int sse)
{
    uint16_t cw = 0, sw = 0;
    int32_t sum = 0;
    unsigned char third[10];
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    check(t, cw == p->fcw, "x87 control word changed");
    check(t, ((sw >> 11) & 7) == 1, "x87 stack top changed");
    x87_compute(&sum, third);
    check(t, sum == e->sum, "x87 sum of the seven values changed");
    check(t, same(third, e->third, 10), "x87 1/3 changed (rounding or precision)");
    if (!sse)
    {
        return;
    }
    static uint32_t out[8][4] __attribute__((aligned(16)));
    uint32_t mxcsr = 0;
    uint32_t quotient[4];
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    __asm__ volatile(
        "movups %%xmm0, 0(%0)\n\t"
        "movups %%xmm1, 16(%0)\n\t"
        "movups %%xmm2, 32(%0)\n\t"
        "movups %%xmm3, 48(%0)\n\t"
        "movups %%xmm4, 64(%0)\n\t"
        "movups %%xmm5, 80(%0)\n\t"
        "movups %%xmm6, 96(%0)\n\t"
        "movups %%xmm7, 112(%0)"
        : : "r"(out) : "memory");
    check(t, mxcsr == p->mxcsr, "MXCSR changed");
    check(t, same(out, p->xmm, sizeof(out)), "XMM registers changed");
    sse_compute(p, quotient);
    check(t, same(quotient, e->quotient, sizeof(quotient)), "divps result changed");
}

// Spins with EDI = marker until the kernel has returned to this program from an interrupt.
static void window(uint32_t marker)
{
    __asm__ volatile(
        "mov %0, %%edi\n\t"
        "push $0\n\t"
        "pop %%gs\n"
        "1:\n\t"
        "mov %%gs, %%ecx\n\t"
        "jecxz 1b\n\t"
        "xor %%edi, %%edi\n\t"
        "jmp 2f\n"
        "2:"
        : : "r"(marker) : "ecx", "edi", "memory");
}

// No syscall between loading the state and checking it.
static void spin_windows(const struct pattern *p, int sse)
{
    struct tally t;
    struct expected e;
    int switched = 0;
    begin(&t, p->who, sse ? "sse windows" : "x87 windows");
    load(p, &e, sse);
    for (int i = 0; i < WINDOWS; i++)
    {
        uint32_t before = rorthox_get_ticks();
        window(0xF0000000U | p->id << 16 | (sse ? 0x55 : 0x87));
        uint32_t after = rorthox_get_ticks();
        if (after - before >= 2)
        {
            switched++;
        }
        verify(&t, p, &e, sse);
    }
    char more[64] = ", ";
    add(add(add_number(add(add_number(more, switched), " of "), WINDOWS), " switched away"), "");
    summary(&t, more);
}

static void syscall_windows(const struct pattern *p)
{
    struct tally t;
    struct expected e;
    begin(&t, p->who, "syscall windows");
    load(p, &e, 1);
    for (int i = 0; i < SLEEPS + RETURNS; i++)
    {
        if (i < SLEEPS)
        {
            sleep(20);
        }
        else
        {
            rorthox_get_ticks();
        }
        verify(&t, p, &e, 1);
    }
    summary(&t, "");
}

// Starts a child with its own state loaded, lets it run, then checks its state again.
static void with_child(const struct pattern *p, const char *command, const char *what, int sse)
{
    struct tally t;
    struct expected e;
    begin(&t, p->who, what);
    load(p, &e, sse);
    rorthox_system_run(command);
    sleep(300);
    verify(&t, p, &e, sse);
    summary(&t, "");
}

static int worker(const struct pattern *p, int driver)
{
    if (driver)
    {
        with_child(p, "fputest.elf fresh x87", "after the x87 child", 0);
        rorthox_system_run("fputest.elf worker");
    }
    spin_windows(p, 0);
    if (driver)
    {
        with_child(p, "fputest.elf fresh sse", "after the sse child", 1);
    }
    spin_windows(p, 1);
    syscall_windows(p);
    return 0;
}

static int fresh(const char *tag, int wait)
{
    static unsigned char image[512] __attribute__((aligned(16)));
    static const unsigned char third[10] = {0xAB, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xFD, 0x3F};
    static const uint32_t ones[4] = {F1, F1, F1, F1};
    static const uint32_t threes[4] = {F3, F3, F3, F3};
    static const unsigned char zero[128];
    struct tally t;
    begin(&t, "fputest fresh", tag);
    memset(image, 0xA5, sizeof(image));
    __asm__ volatile("fxsave %0" : "=m"(image));
    check(&t, image[0] == 0x7F && image[1] == 0x03, "control word is not 0x037F");
    check(&t, image[2] == 0 && image[3] == 0, "status word is not 0");
    check(&t, image[4] == 0, "x87 registers not all empty");
    check(&t, image[24] == 0x80 && image[25] == 0x1F && image[26] == 0 && image[27] == 0, "MXCSR is not 0x1F80");
    int st = 1;
    for (int i = 0; i < 8; i++)
    {
        st &= same(image + 32 + 16 * i, zero, 10);
    }
    check(&t, st, "x87 register contents not zero");
    check(&t, same(image + 160, zero, 128), "XMM registers not zero");
    unsigned char result[10];
    __asm__ volatile("fld1\n\tfidivl %1\n\tfstpt %0" : "=m"(result) : "m"(three));
    check(&t, same(result, third, 10), "x87 1/3 is not the round-to-nearest 64-bit result");
    uint32_t quotient[4];
    uint32_t mxcsr = 0;
    __asm__ volatile(
        "movups (%1), %%xmm0\n\t"
        "movups (%2), %%xmm1\n\t"
        "divps %%xmm1, %%xmm0\n\t"
        "movups %%xmm0, (%0)"
        : : "r"(quotient), "r"(ones), "r"(threes) : "memory");
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    check(&t, quotient[0] == 0x3EAAAAABU && quotient[3] == 0x3EAAAAABU, "divps 1/3 is not rounded to nearest");
    check(&t, mxcsr == 0x1FA0, "MXCSR after divps is not 0x1FA0");
    summary(&t, "");
    if (wait)
    {
        say(t.who, "waiting for a key");
        rorthox_getkeyblock();
    }
    return 0;
}

// Loads I, waits for a key, then loads a last XMM7 ("EXIT FINAL STATE" or "FAULT FINAL STAT") and
// ends with exit / a page fault right after it. The load and the end are in one basic block, so
// no interrupt comes between them under TCG (and only within a few instructions on a real CPU):
// the kernel never saves that last state while the program lives.
static int dirty(const char *who, int fault)
{
    static const char exit_state[16] = "EXIT FINAL STATE";
    static const char fault_state[16] = "FAULT FINAL STAT";
    struct expected e;
    load(&pattern_i, &e, 1);
    say(who, "loaded, waiting for a key");
    rorthox_getkeyblock();
    if (fault)
    {
        __asm__ volatile("movups (%0), %%xmm7\n\tmovl $1, 0" : : "r"(fault_state) : "memory");
        say(who, "FAIL no page fault");
    }
    __asm__ volatile("movups (%0), %%xmm7\n\tmov %1, %%eax\n\tint %2"
                     : : "r"(exit_state), "i"(SYSCALL_EXIT), "i"(RORTHOX_SYSCALL_VECTOR) : "eax", "memory");
    return 0;
}

static int x87_error(void)
{
    static const uint16_t cw = 0x037B;   // zero-divide unmasked
    uint16_t sw = 0;
    __asm__ volatile("fninit\n\tfldcw %1\n\tfld1\n\tfidivl %2\n\tfnstsw %0" : "=m"(sw) : "m"(cw), "m"(zero_divisor));
    // ZE and ES: the error is pending, and only a waiting instruction reports it.
    say("fputest mf", (sw & 0x84) == 0x84 ? "error pending" : "FAIL no pending error");
    __asm__ volatile("fwait");
    say("fputest mf", "FAIL no exception at fwait");
    return 0;
}

// The first program, alone: with it asleep the kernel idles.
static int idle(void)
{
    struct tally t;
    struct expected e;
    const struct pattern *p = &pattern_i;
    begin(&t, p->who, "idle");
    load(p, &e, 1);
    say(p->who, "loaded, sleeping until a key");
    while (!rorthox_getkey())
    {
        sleep(100);
    }
    verify(&t, p, &e, 1);
    summary(&t, "");
    with_child(p, "shell.elf fresh idle", "after the child", 1);
    while (1)
    {
        sleep(1000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 0)
    {
        return idle();
    }
    const char *mode = argc > 1 ? argv[1] : "";
    if (argc == 1)
    {
        return worker(&pattern_a, 1);
    }
    if (strncmp(mode, "worker", 7) == 0)
    {
        return worker(&pattern_b, 0);
    }
    if (strncmp(mode, "fresh", 6) == 0 && argc > 2)
    {
        return fresh(argv[2], 0);
    }
    if (strncmp(mode, "afterexit", 10) == 0 || strncmp(mode, "afterfault", 11) == 0)
    {
        return fresh(mode, 1);
    }
    if (strncmp(mode, "dirty", 6) == 0)
    {
        return dirty("fputest dirty", 0);
    }
    if (strncmp(mode, "fault", 6) == 0)
    {
        return dirty("fputest fault", 1);
    }
    if (strncmp(mode, "mf", 3) == 0)
    {
        return x87_error();
    }
    say("fputest", "FAIL unknown mode");
    return 0;
}
