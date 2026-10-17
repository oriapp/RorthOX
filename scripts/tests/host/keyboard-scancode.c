/* Exercise the registered IRQ handler, not its lookup implementation. Expected keys are
 * independent of the driver's tables. ASan checks every input byte in every modifier state.
 * The handler never switches the page directory or segments itself (interrupt_handler does,
 * and only for an IRQ from ring 3): no kernel_page or task_page call on any byte. */
#include <stdint.h>
#include <stddef.h>
#include "keyboard/classic.c"
#include "host.h"

static const uint8_t regular[256] = {
    [0x01] = 0x1b,
    [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x08, '\t',
    [0x10] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0x0d,
    [0x1e] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    [0x2b] = '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    [0x37] = '*', [0x39] = ' ',
    [0x47] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};
static const uint8_t shifted[256] = {
    [0x01] = 0x1b,
    [0x02] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x08, '\t',
    [0x10] = 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0x0d,
    [0x1e] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    [0x2b] = '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    [0x37] = '*', [0x39] = ' ',
    [0x47] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};
/* Makes after E0. Breaks (second byte | 0x80) give nothing. */
static const int extended_keys[256] = {
    [0x1c] = 0x0d, [0x35] = '/',
    [0x48] = RORTHOX_KEY_UP, [0x50] = RORTHOX_KEY_DOWN, [0x4b] = RORTHOX_KEY_LEFT, [0x4d] = RORTHOX_KEY_RIGHT,
    [0x47] = RORTHOX_KEY_HOME, [0x4f] = RORTHOX_KEY_END, [0x52] = RORTHOX_KEY_INSERT, [0x53] = RORTHOX_KEY_DELETE,
    [0x49] = RORTHOX_KEY_PAGE_UP, [0x51] = RORTHOX_KEY_PAGE_DOWN,
};
/* Keypad 7 8 9 4 5 6 1 2 3 0 . : nothing with Num Lock off. */
static const uint8_t keypad_digit[256] = {
    [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4b] = 1, [0x4c] = 1, [0x4d] = 1,
    [0x4f] = 1, [0x50] = 1, [0x51] = 1, [0x52] = 1, [0x53] = 1,
};

static int checks, failed, input, reads, pushed, value;
static int model_caps, model_left, model_right, model_extended;
static int model_numlock = 1, model_numlock_down, model_pause;
static struct keyboard *device;
static INTERRUPT_CALLBACK_FUNCTION irq;

static void expect(int ok, const char *what, int code, int got, int want)
{
    checks++;
    if (!ok && ++failed <= 20)
        printf("FAIL %s: code=%02x caps=%d left=%d right=%d num=%d got=%02x want=%02x\n",
               what, code, model_caps, model_left, model_right, model_numlock, got, want);
}

unsigned char insb(unsigned short port)
{
    expect(port == 0x60, "input port", input, port, 0x60);
    reads++;
    return input;
}
// Every port write: init enables the first PS/2 port (0x64 <- 0xAE) and nothing else is sent,
// no LED command (0xED) and no other keyboard command.
static int outb_calls, outb_bad;
void outb(unsigned short port, unsigned char val)
{
    if (outb_calls++ || port != 0x64 || val != 0xae)
        outb_bad++;
}
int idt_register_interrupt_callback(int vector, INTERRUPT_CALLBACK_FUNCTION callback)
{
    expect(vector == ISR_KEYBOARD_INTERRUPT, "IRQ1 vector", 0, vector, ISR_KEYBOARD_INTERRUPT);
    irq = callback;
    return 0;
}
void keyboard_set_capslock(struct keyboard *keyboard, KEYBOARD_CAPS_LOCK_STATE state)
{
    keyboard->capslock_state = state;
}
KEYBOARD_CAPS_LOCK_STATE keyboard_get_capslock(struct keyboard *keyboard)
{
    return keyboard->capslock_state;
}
// Everything pushed since the last log_clear(), for the whole-sequence checks.
static int log_keys[32], log_count;
static void log_clear(void) { log_count = 0; }
void keyboard_push(uint16_t key)
{
    pushed++;
    value = key;
    if (log_count < 32)
        log_keys[log_count] = key;
    log_count++;
}
// The IRQ1 callback must leave both to interrupt_handler.
static int page_switches;
void kernel_page() { page_switches++; }
int task_page() { page_switches++; return 0; }

static void send(int code)
{
    int want = 0;
    if (model_pause)
        model_pause--; /* E1 drops the two bytes after it */
    else if (model_extended)
    {
        model_extended = 0;
        if (!(code & 0x80))
            want = extended_keys[code];
    }
    else if (code == 0xe0)
        model_extended = 1;
    else if (code == 0xe1)
        model_pause = 2;
    else
    {
        if (code == 0x3a) model_caps = !model_caps;
        if (code == 0x2a) model_left = 1;
        if (code == 0xaa) model_left = 0;
        if (code == 0x36) model_right = 1;
        if (code == 0xb6) model_right = 0;
        if (code == 0x45)
        {
            if (!model_numlock_down) model_numlock = !model_numlock;
            model_numlock_down = 1;
        }
        if (code == 0xc5) model_numlock_down = 0;
        int shift = model_left || model_right;
        want = shift ? shifted[code] : regular[code];
        if (regular[code] >= 'a' && regular[code] <= 'z')
            want = regular[code] - ((shift != model_caps) ? 32 : 0);
        if (!model_numlock && keypad_digit[code])
            want = 0;
    }
    input = code;
    reads = pushed = value = page_switches = 0;
    irq(NULL); /* the driver doesn't read the frame */
    expect(reads == 1, "one port read", code, reads, 1);
    expect(page_switches == 0, "no kernel_page/task_page", code, page_switches, 0);
    expect(pushed == (want != 0), "push count", code, pushed, want != 0);
    expect(value == want, "key", code, value, want);
    int caps = keyboard_get_capslock(device);
    expect(caps == model_caps, "caps state", code, caps, model_caps);
}

/* Release both shifts and Num Lock, set Caps, then put Num Lock in the wanted state with a full press. */
static void reset(int caps, int num)
{
    send(0xaa);
    send(0xb6);
    send(0xc5);
    keyboard_set_capslock(device, caps);
    model_caps = caps;
    if (model_numlock != num)
    {
        send(0x45);
        send(0xc5);
    }
}

int main(void)
{
    device = classic_init();
    if (!device || !device->init || device->init() || !irq)
        return 2;
    /* Num Lock is on after init: the model starts on, so keypad 1 must give '1'. */
    send(0x4f); send(0xcf);

    for (int num = 0; num < 2; num++)
        for (int caps = 0; caps < 2; caps++)
            for (int shifts = 0; shifts < 4; shifts++)
                for (int code = 0; code < 256; code++)
                {
                    // Reset through real release events; no dependency on private modifier fields.
                    reset(caps, num);
                    if (shifts & 1) send(0x2a);
                    if (shifts & 2) send(0x36);
                    send(code);
                    // Pin the resulting held/caps/Num Lock state, including both-shift release ordering.
                    send(0x1e);
                    send(0x9e);
                    send(0x4f);
                    send(0xcf);
                }
    printf("interrupt: all 256 values, Num Lock off/on, caps off/on, neither/left/right/both shifts\n");

    reset(0, 1);
    send(0x2a); send(0x2a); send(0x1e); // repeated make must not toggle Shift
    send(0x36); send(0xaa); send(0x1e); // right still held
    send(0xb6); send(0x1e);
    send(0x36); send(0x2a); send(0xb6); send(0x1e); // left still held
    send(0x3a); send(0xba); send(0x1e); // Caps+Shift is lowercase
    send(0xaa); send(0x1e); send(0x3a); send(0xba); send(0x1e);
    printf("interrupt: modifier transitions checked\n");

    /* Held Num Lock repeats its make: one toggle per press. A break alone changes nothing. */
    reset(0, 1);
    send(0x45); send(0x4f);
    send(0x45); send(0x4f);
    send(0x45); send(0x4f);
    expect(model_numlock == 0 && pushed == 0, "Num Lock held toggles once", 0x45, pushed, 0);
    send(0xc5); send(0xc5); send(0x4f);
    expect(pushed == 0, "Num Lock break does not toggle", 0xc5, pushed, 0);
    send(0x45); send(0xc5); send(0x4f);
    expect(value == '1', "Num Lock second press", 0x45, value, '1');
    printf("interrupt: Num Lock press cycles checked\n");

    /* Pause: E1 1D 45 E1 9D C5 changes no state, in both Num Lock states. */
    for (int num = 0; num < 2; num++)
    {
        reset(0, num);
        const int pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
        for (int i = 0; i < 6; i++)
            send(pause[i]);
        send(0x4f);
        expect(value == (num ? '1' : 0), "keypad after Pause", 0x4f, value, num ? '1' : 0);
        send(0x1e);
        expect(value == 'a', "letter after Pause", 0x1e, value, 'a');
    }
    printf("interrupt: Pause checked\n");

    for (int num = 0; num < 2; num++)
        for (int caps = 0; caps < 2; caps++)
            for (int shifts = 0; shifts < 4; shifts++)
                for (int code = 0; code < 256; code++)
                {
                    reset(caps, num);
                    if (shifts & 1) send(0x2a);
                    if (shifts & 2) send(0x36);
                    send(0xe0); send(code);
                    // Fake shifts and releases must preserve each held key independently.
                    send(0x1e);
                    send(0xaa); send(0x1e);
                    send(0xb6); send(0x1e);
                    send(0x4f); send(0xcf);
                }
    printf("interrupt: E0 plus all 256 values in every Num Lock/Caps/Shift state checked\n");

    const int navigation[] = {0x48, 0x50, 0x4b, 0x4d, 0x47, 0x4f, 0x52, 0x53, 0x49, 0x51};
    for (int num = 0; num < 2; num++)
    {
        reset(0, num);
        for (int i = 0; i < 10; i++)
        {
            send(0xe0); send(navigation[i]);
            expect(value >= RORTHOX_KEY_UP && value <= RORTHOX_KEY_PAGE_DOWN, "navigation key value",
                   navigation[i], value, extended_keys[navigation[i]]);
            send(0xe0); send(navigation[i] | 0x80);
        }
    }
    printf("interrupt: E0 navigation keys give their key values, breaks give nothing\n");

    /* Whole sequences a real keyboard sends (QEMU sends none of the fake shifts or a cut-off
     * Pause): only the listed keys come out, and held Shift and Num Lock are what they were. */
    static const struct
    {
        const char *name;
        int num, bytes[24], nbytes, want[4], nwant;
    } sequences[] = {
        {"Up with Num Lock fake shifts, then a", 1,
         {0xe0, 0x2a, 0xe0, 0x48, 0xe0, 0xc8, 0xe0, 0xaa, 0x1e, 0x9e}, 10, {RORTHOX_KEY_UP, 'a'}, 2},
        {"left Shift held, Up with fake shift release/press", 0,
         {0x2a, 0xe0, 0xaa, 0xe0, 0x48, 0xe0, 0xc8, 0xe0, 0x2a, 0x1e, 0x9e, 0xaa, 0x1e, 0x9e}, 14,
         {RORTHOX_KEY_UP, 'A', 'a'}, 3},
        {"right Shift held, End with fake shifts", 0,
         {0x36, 0xe0, 0xb6, 0xe0, 0x4f, 0xe0, 0xcf, 0xe0, 0x36, 0x1e, 0x9e, 0xb6, 0x1e, 0x9e}, 14,
         {RORTHOX_KEY_END, 'A', 'a'}, 3},
        {"both Shifts held, Home with both fake shifts", 0,
         {0x2a, 0x36, 0xe0, 0xaa, 0xe0, 0xb6, 0xe0, 0x47, 0xe0, 0xc7, 0xe0, 0x36, 0xe0, 0x2a, 0x1e, 0x9e,
          0xaa, 0x1e, 0x9e, 0xb6, 0x1e, 0x9e}, 22, {RORTHOX_KEY_HOME, 'A', 'A', 'a'}, 4},
        {"left Shift held, keypad / with fake shifts", 1,
         {0x2a, 0xe0, 0xaa, 0xe0, 0x35, 0xe0, 0xb5, 0xe0, 0x2a, 0x1e, 0x9e, 0xaa}, 12, {'/', 'A'}, 2},
        {"Print Screen make and break, then keypad 1 and a", 1,
         {0xe0, 0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0xe0, 0xaa, 0x4f, 0xcf, 0x1e, 0x9e}, 12, {'1', 'a'}, 2},
        {"Shift+Print Screen, then a", 1,
         {0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0x1e, 0x9e, 0xaa, 0x1e}, 9, {'A', 'a'}, 2},
        {"Alt+Print Screen (QEMU's fake Alt), then a", 1,
         {0xb8, 0x38, 0x54, 0xd4, 0xb8, 0x38, 0x1e}, 7, {'a'}, 1},
        {"Ctrl+Pause, then keypad 1", 1,
         {0x1d, 0xe0, 0x46, 0xe0, 0xc6, 0x9d, 0x4f}, 7, {'1'}, 1},
        {"Pause cut off after 1D, then a, a", 1,
         {0xe1, 0x1d, 0x1e, 0x9e, 0x1e}, 5, {'a'}, 1},
        {"Pause cut off after 45, then keypad 1", 1,
         {0xe1, 0x1d, 0x45, 0x4f}, 4, {'1'}, 1},
        {"Pause in the byte after E0, then keypad 1", 1,
         {0xe0, 0xe1, 0x4f}, 3, {'1'}, 1},
    };
    for (unsigned s = 0; s < sizeof(sequences) / sizeof(sequences[0]); s++)
    {
        reset(0, sequences[s].num);
        log_clear();
        for (int i = 0; i < sequences[s].nbytes; i++)
            send(sequences[s].bytes[i]);
        int ok = log_count == sequences[s].nwant;
        for (int i = 0; ok && i < log_count; i++)
            ok = log_keys[i] == sequences[s].want[i];
        expect(ok, sequences[s].name, s, log_count, sequences[s].nwant);
    }
    printf("interrupt: fake shifts, Print Screen, Ctrl+Pause and cut-off Pause sequences checked\n");

    expect(outb_calls == 1 && !outb_bad, "only the port enable command was written", 0, outb_calls, 1);
    printf("ports: init wrote 0xAE to 0x64, nothing else was written\n");
    printf("keyboard-scancode host check: %d of %d checks failed\n", failed, checks);
    if (!failed) printf("keyboard-scancode host check: passed\n");
    fflush(0);
    return failed ? 1 : 0;
}
