/* Exercise the registered IRQ handler, not its lookup implementation. Expected bytes are
 * independent of the driver's tables. ASan checks every input byte in every modifier state. */
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

static int checks, failed, input, reads, pushed, value;
static int model_caps, model_left, model_right, model_extended;
static struct keyboard *device;
static INTERRUPT_CALLBACK_FUNCTION irq;

static void expect(int ok, const char *what, int code, int got, int want)
{
    checks++;
    if (!ok && ++failed <= 20)
        printf("FAIL %s: code=%02x caps=%d left=%d right=%d got=%02x want=%02x\n",
               what, code, model_caps, model_left, model_right, got, want);
}

unsigned char insb(unsigned short port)
{
    expect(port == 0x60, "input port", input, port, 0x60);
    reads++;
    return input;
}
void outb(unsigned short port, unsigned char val) {}
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
void keyboard_push(char c) { pushed++; value = (uint8_t)c; }
void kernel_page() {}
int task_page() { return 0; }

static void send(int code)
{
    int want = 0;
    if (model_extended)
    {
        model_extended = 0;
        if (code == 0x1c) want = 0x0d;
        if (code == 0x35) want = '/';
    }
    else if (code == 0xe0)
        model_extended = 1;
    else
    {
        if (code == 0x3a) model_caps = !model_caps;
        if (code == 0x2a) model_left = 1;
        if (code == 0xaa) model_left = 0;
        if (code == 0x36) model_right = 1;
        if (code == 0xb6) model_right = 0;
        int shift = model_left || model_right;
        want = shift ? shifted[code] : regular[code];
        if (regular[code] >= 'a' && regular[code] <= 'z')
            want = regular[code] - ((shift != model_caps) ? 32 : 0);
    }
    input = code;
    reads = pushed = value = 0;
    irq();
    expect(reads == 1, "one port read", code, reads, 1);
    expect(pushed == (want != 0), "push count", code, pushed, want != 0);
    expect(value == want, "character", code, value, want);
    int caps = keyboard_get_capslock(device);
    expect(caps == model_caps, "caps state", code, caps, model_caps);
}

int main(void)
{
    device = classic_init();
    if (!device || !device->init || device->init() || !irq)
        return 2;
    for (int caps = 0; caps < 2; caps++)
        for (int shifts = 0; shifts < 4; shifts++)
            for (int code = 0; code < 256; code++)
            {
                // Reset through real release events; no dependency on private modifier fields.
                send(0xaa);
                send(0xb6);
                keyboard_set_capslock(device, caps);
                model_caps = caps;
                if (shifts & 1) send(0x2a);
                if (shifts & 2) send(0x36);
                send(code);
                // Pin the resulting held/caps state, including both-shift release ordering.
                send(0x1e);
                send(0x9e);
            }
    printf("interrupt: all 256 values, caps off/on, neither/left/right/both shifts\n");

    send(0xaa); send(0xb6);
    keyboard_set_capslock(device, 0); model_caps = 0;
    send(0x2a); send(0x2a); send(0x1e); // repeated make must not toggle Shift
    send(0x36); send(0xaa); send(0x1e); // right still held
    send(0xb6); send(0x1e);
    send(0x36); send(0x2a); send(0xb6); send(0x1e); // left still held
    send(0x3a); send(0xba); send(0x1e); // Caps+Shift is lowercase
    send(0xaa); send(0x1e); send(0x3a); send(0xba); send(0x1e);
    printf("interrupt: modifier transitions checked\n");

    for (int caps = 0; caps < 2; caps++)
        for (int shifts = 0; shifts < 4; shifts++)
            for (int code = 0; code < 256; code++)
            {
                send(0xaa); send(0xb6);
                keyboard_set_capslock(device, caps); model_caps = caps;
                if (shifts & 1) send(0x2a);
                if (shifts & 2) send(0x36);
                send(0xe0); send(code);
                // Fake shifts and releases must preserve each held key independently.
                send(0x1e);
                send(0xaa); send(0x1e);
                send(0xb6); send(0x1e);
            }
    printf("interrupt: E0 plus all 256 values in every Caps/Shift state checked\n");
    const int arrows[] = {0x48, 0x50, 0x4b, 0x4d};
    for (int i = 0; i < 4; i++)
    {
        send(0xe0); send(arrows[i]);
        send(0xe0); send(arrows[i] | 0x80);
    }
    printf("interrupt: extended arrows push nothing\n");
    printf("keyboard-scancode host check: %d of %d checks failed\n", failed, checks);
    if (!failed) printf("keyboard-scancode host check: passed\n");
    fflush(0);
    return failed ? 1 : 0;
}
