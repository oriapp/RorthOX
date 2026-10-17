/*
 * Host check of the PIT driver (kernel/arch/i686/pit.c) and its IRQ0 tick through the real
 * interrupt dispatcher (interrupt_handler in kernel/idt/idt.c), for scripts/tests/pit.sh. Both
 * files are included here; the linker drops what the checks don't use.
 *
 *   init      the port writes are exactly 0x43 <- 0x36, 0x40 <- 0x9c, 0x40 <- 0x2e: channel 0,
 *             mode 3, divisor 11932, so a tick is at least 10 ms of the 1193182 Hz clock; the
 *             tick is registered for vector 0x20 and nothing else
 *   convert   pit_ms_to_ticks against an independent model (0 for 0, else ceil(ms / 10) + 2,
 *             in 64-bit arithmetic) for 0..100000, the decimal edges and INT32_MAX
 *   tick-3    IRQ0 from ring 3: kernel_page, save the task, EOI, count, wake the sleepers with
 *             the new count, task_next (which doesn't return); one EOI
 *   tick-0    IRQ0 from ring 0 (the idle loop) with a frame that ends after flags, right before
 *             an unmapped page: kernel_page, count, wake with the new count, one EOI, return;
 *             no task save, no task_page, no task_next, the frame unchanged
 *   irq-3/0   IRQ1 with a callback and IRQ7 without one, from ring 3 and ring 0: ring 3 saves
 *             the task (only with a callback) and calls task_page, ring 0 does neither; one
 *             EOI each
 *
 * Prints "PASS <case>" or "FAIL <case>" per case and "pit host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include "idt/idt.c"
#include "arch/i686/pit.c"
#include "host.h"

void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int mprotect(void *addr, unsigned long length, int prot);
#define HOST_PROT_NONE 0
#define HOST_PROT_RW 3
#define HOST_MAP_PRIVATE_ANON 0x22

// Everything the dispatcher and the tick did, in order: K kernel_page, S save (with the frame),
// E EOI to the master, e EOI to the slave, W wake (with its argument), N task_next, P task_page,
// C the test callback.
static char events[64];
static int event_count;
static uint32_t wake_now[8];
static int wakes;
static struct interrupt_frame *saved_frame;
static uint32_t jiffies_at_eoi, jiffies_at_next;
static jmp_buf scheduled;
static int in_tick;

static void event(char c)
{
    if (event_count < (int)sizeof(events) - 1)
        events[event_count++] = c;
    events[event_count] = 0;
}

// Port writes: the PIT's while pit_init runs, EOIs after.
static unsigned short init_ports[8];
static unsigned char init_values[8];
static int init_writes, initialising;

void outb(unsigned short port, unsigned char value)
{
    if (initialising)
    {
        if (init_writes < 8)
        {
            init_ports[init_writes] = port;
            init_values[init_writes] = value;
        }
        init_writes++;
        return;
    }
    if (port == 0x20 && value == 0x20)
    {
        jiffies_at_eoi = jiffies;
        event('E');
    }
    else if (port == 0xA0 && value == 0x20)
        event('e');
    else
        event('?');
}

void kernel_page() { event('K'); }
int task_page() { event('P'); return 0; }
void task_current_save_state(struct interrupt_frame *frame)
{
    saved_frame = frame;
    event('S');
}
void task_wake_sleepers(uint32_t now)
{
    if (wakes < 8)
        wake_now[wakes] = now;
    wakes++;
    event('W');
}
void task_next()
{
    event('N');
    jiffies_at_next = jiffies;
    if (in_tick)
        longjmp(scheduled, 1); // like the real switch: never back into the callback
}

static void test_callback(struct interrupt_frame *frame) { event('C'); }

static int failed, cases;

static void report(const char *name, int ok)
{
    cases++;
    if (!ok)
        failed++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static void reset(void)
{
    event_count = 0;
    events[0] = 0;
    wakes = 0;
    saved_frame = 0;
    jiffies_at_eoi = jiffies_at_next = 0xdeadbeef;
}

static uint32_t model_ticks(uint32_t ms)
{
    if (ms == 0)
        return 0;
    return (uint32_t)(((unsigned long long)ms * 100 + 999) / 1000) + 2;
}

static int text_same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

// A ring 0 frame is 48 bytes (it ends after flags); put it right before an unmapped page.
static struct interrupt_frame *ring0_frame(void)
{
    unsigned char *pages = mmap(0, 8192, HOST_PROT_RW, HOST_MAP_PRIVATE_ANON, -1, 0);
    if (pages == (void *)-1 || mprotect(pages + 4096, 4096, HOST_PROT_NONE))
    {
        perror("mmap");
        exit(2);
    }
    struct interrupt_frame *frame = (struct interrupt_frame *)(pages + 4096 - 48);
    unsigned char *p = (unsigned char *)frame;
    for (int i = 0; i < 48; i++)
        p[i] = (unsigned char)(0xa0 + i);
    frame->cs = 0x08;
    frame->flags = 0x202;
    return frame;
}

static int same_bytes(const void *a, const void *b, int n)
{
    const unsigned char *x = a, *y = b;
    for (int i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

// One dispatcher call from ring 0: must come back.
static int ring0_irq(int vector, struct interrupt_frame *frame, const char *want)
{
    unsigned char before[48];
    for (int i = 0; i < 48; i++)
        before[i] = ((unsigned char *)frame)[i];
    reset();
    in_tick = 1;
    if (setjmp(scheduled))
    {
        in_tick = 0;
        printf("  vector 0x%x from ring 0 switched tasks: %s\n", vector, events);
        return 0;
    }
    interrupt_handler(vector, frame, 0);
    in_tick = 0;
    int ok = text_same(events, want) && !saved_frame && same_bytes(before, frame, 48);
    if (!ok)
        printf("  vector 0x%x from ring 0: %s, expected %s%s\n", vector, events, want,
               same_bytes(before, frame, 48) ? "" : " (frame changed)");
    return ok;
}

// One dispatcher call from ring 3 that must come back (no task switch).
static int ring3_irq(int vector, const char *want, int want_save)
{
    struct interrupt_frame frame = { .cs = 0x1b, .flags = 0x202, .esp = 0x3ff000, .ss = 0x23 };
    reset();
    in_tick = 1;
    if (setjmp(scheduled))
    {
        in_tick = 0;
        printf("  vector 0x%x from ring 3 switched tasks: %s\n", vector, events);
        return 0;
    }
    interrupt_handler(vector, &frame, 0);
    in_tick = 0;
    int ok = text_same(events, want) && saved_frame == (want_save ? &frame : 0);
    if (!ok)
        printf("  vector 0x%x from ring 3: %s, expected %s\n", vector, events, want);
    return ok;
}

int main(void)
{
    // init: the PIT programming and the registration.
    initialising = 1;
    pit_init();
    initialising = 0;
    int registered = 0;
    for (int v = 0; v < RORTHOX_TOTAL_INTERRUPTS; v++)
        registered += interrupt_callbacks[v] != 0;
    unsigned divisor = init_writes == 3 ? init_values[1] | (init_values[2] << 8) : 0;
    int ok = init_writes == 3 && init_ports[0] == 0x43 && init_values[0] == 0x36 && init_ports[1] == 0x40 &&
             init_ports[2] == 0x40 && divisor == 11932 && divisor * 100 >= 1193182 && registered == 1 &&
             interrupt_callbacks[RORTHOX_PIC_MASTER_VECTOR_BASE] == pit_tick;
    for (int i = 0; i < init_writes && i < 8; i++)
        printf("  outb(0x%x, 0x%x)\n", init_ports[i], init_values[i]);
    if (ok)
        printf("PIT: divisor 11932, mode 3, IRQ0, at least 10 ms per tick\n");
    report("init", ok);

    // convert
    static const uint32_t edges[] = { 0, 1, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 1009, 1010, 1011,
                                       59999, 60000, 60001, 0x7FFFFFFEu, 0x7FFFFFFFu };
    ok = 1;
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
    {
        uint32_t got = pit_ms_to_ticks(edges[i]);
        printf("  %u ms -> %u ticks\n", edges[i], got);
        if (got != model_ticks(edges[i]))
        {
            printf("  %u ms: %u ticks, expected %u\n", edges[i], got, model_ticks(edges[i]));
            ok = 0;
        }
    }
    for (uint32_t ms = 0; ms <= 100000; ms++)
    {
        if (pit_ms_to_ticks(ms) != model_ticks(ms))
        {
            printf("  %u ms: %u ticks, expected %u\n", ms, pit_ms_to_ticks(ms), model_ticks(ms));
            ok = 0;
            break;
        }
    }
    ok &= pit_ms_to_ticks(0x7FFFFFFFu) == 214748367u;
    report("convert", ok);

    // tick-3: from a user task. The real task switch doesn't return.
    struct interrupt_frame user = { .cs = 0x1b, .flags = 0x202, .esp = 0x3ff000, .ss = 0x23 };
    jiffies = 41;
    reset();
    in_tick = 1;
    int switched = 0;
    if (setjmp(scheduled))
        switched = 1;
    else
        interrupt_handler(RORTHOX_PIC_MASTER_VECTOR_BASE, &user, 0);
    in_tick = 0;
    ok = switched && text_same(events, "KSEWN") && saved_frame == &user && jiffies == 42 && jiffies_at_eoi == 41 &&
         wakes == 1 && wake_now[0] == 42 && jiffies_at_next == 42;
    printf("  ring 3 tick: %s, jiffies 41 -> %u, wake(%u)\n", events, jiffies, wakes ? wake_now[0] : 0);
    report("tick-3", ok);

    // tick-0: from the idle loop, twice (no growth of anything, one EOI each).
    struct interrupt_frame *idle = ring0_frame();
    jiffies = 0xFFFFFFFFu;
    ok = ring0_irq(RORTHOX_PIC_MASTER_VECTOR_BASE, idle, "KWE") && jiffies == 0 && wakes == 1 && wake_now[0] == 0 &&
         jiffies_at_eoi == 0;
    printf("  ring 0 tick: %s, jiffies 0xffffffff -> %u, wake(%u)\n", events, jiffies, wakes ? wake_now[0] : 0);
    ok &= ring0_irq(RORTHOX_PIC_MASTER_VECTOR_BASE, idle, "KWE") && jiffies == 1 && wakes == 1 && wake_now[0] == 1;
    report("tick-0", ok);

    // irq-3 / irq-0: IRQ1 with a callback, IRQ7 without one.
    INTERRUPT_CALLBACK_FUNCTION keep = interrupt_callbacks[RORTHOX_PIC_MASTER_VECTOR_BASE + 1];
    idt_register_interrupt_callback(RORTHOX_PIC_MASTER_VECTOR_BASE + 1, test_callback);
    ok = ring3_irq(RORTHOX_PIC_MASTER_VECTOR_BASE + 1, "KSCPE", 1);
    ok &= ring3_irq(RORTHOX_PIC_MASTER_VECTOR_BASE + 7, "KPE", 0);
    report("irq-3", ok);
    ok = ring0_irq(RORTHOX_PIC_MASTER_VECTOR_BASE + 1, idle, "KCE");
    ok &= ring0_irq(RORTHOX_PIC_MASTER_VECTOR_BASE + 7, idle, "KE");
    report("irq-0", ok);
    interrupt_callbacks[RORTHOX_PIC_MASTER_VECTOR_BASE + 1] = keep;

    printf("pit host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
