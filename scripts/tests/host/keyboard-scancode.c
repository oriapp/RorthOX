/*
 * Host check for scripts/tests/keyboard-scancode.sh: the real kernel/keyboard/classic.c, built
 * with the host C compiler and AddressSanitizer, so a read past the end of its scancode table
 * stops the check with a report instead of returning whatever byte follows the table.
 * The ports, the interrupt registration, paging and keyboard_push() are stubs.
 *
 * The table has 82 entries today (0x00-0x51): it has no entries for NumLock (0x45) and
 * ScrollLock (0x46), so the keypad keys after them sit two places early. That is a separate
 * problem: this check takes the table size from the table, checks the keys up to 0x44 against
 * scancode set 1, and the keypad range only against the table itself.
 *
 * 1. classic_keyboard_scancode_to_char() for all 256 byte values, caps lock off and on: values
 *    inside the table give its character (letters lower case with caps lock off), every value
 *    from the table size up gives 0. Backspace, Enter, the last entry and the first value past
 *    the table by name.
 * 2. classic_keyboard_handle_interrupt() for all 256 byte values read from port 0x60: a press
 *    with a character pushes exactly that character, anything else pushes nothing; caps lock,
 *    left shift and the left shift release toggle caps lock as before; port 0x60 is read once.
 *
 * Prints "keyboard-scancode host check: <failed> of <checks> checks failed".
 */
#include <stdint.h>
#include <stddef.h>

#include "keyboard/classic.c"

#include "host.h"

// Scancode set 1, keys 0x00-0x44 (with caps lock on): what classic.c must give for them.
#define SET_ONE_CHECKED 0x45
static const uint8_t set_one[SET_ONE_CHECKED] = {
    0x00, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x08, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '[', ']', 0x0D, 0x00, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ';', '\'', '`', 0x00, '\\', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ',', '.', '/', 0x00, '*', 0x00, ' ', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

#define TABLE_SIZE ((int)sizeof(keyboard_scan_set_one))

static int checks;
static int failed;

static void expect(int ok, const char *what, int scancode, int caps, int got, int want)
{
    checks++;
    if (!ok)
    {
        failed++;
        if (failed <= 20)
        {
            printf("  FAIL %s: scancode 0x%02x caps %d: got 0x%02x, expected 0x%02x\n", what, scancode, caps, got,
                   want);
        }
    }
}

static uint8_t expected_char(int scancode, int caps)
{
    if (scancode >= TABLE_SIZE)
    {
        return 0;
    }
    // Only read below the table size, so the expectation itself never reads past the table.
    uint8_t c = scancode < SET_ONE_CHECKED ? set_one[scancode] : keyboard_scan_set_one[scancode];
    if (!caps && c >= 'A' && c <= 'Z')
    {
        c += 32;
    }
    return c;
}

// Stubs for what classic.c calls.
static int port60_value;
static int port60_reads;
static int pushed_count;
static int pushed_char;

unsigned char insb(unsigned short port)
{
    if (port == KEYBOARD_INPUT_PORT)
    {
        port60_reads++;
        return (unsigned char)port60_value;
    }
    return 0;
}

void outb(unsigned short port, unsigned char val)
{
}

int idt_register_interrupt_callback(int interrupt, INTERRUPT_CALLBACK_FUNCTION interrupt_callback)
{
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

void keyboard_push(char c)
{
    pushed_count++;
    pushed_char = (uint8_t)c;
}

void kernel_page()
{
}

int task_page()
{
    return 0;
}

static int lookup(int scancode, int caps)
{
    keyboard_set_capslock(&classic_keyboard, caps ? KEYBOARD_CAPS_LOCK_ON : KEYBOARD_CAPS_LOCK_OFF);
    return classic_keyboard_scancode_to_char((uint8_t)scancode);
}

int main(void)
{
    int caps;
    int scancode;
    int got;

    printf("table size: %d entries, last 0x%02x\n", TABLE_SIZE, TABLE_SIZE - 1);
    fflush(0);
    expect(TABLE_SIZE > SET_ONE_CHECKED && TABLE_SIZE <= 0x80, "table size", TABLE_SIZE, 0, TABLE_SIZE, 0);

    // 1. The lookup itself.
    for (caps = 0; caps <= 1; caps++)
    {
        for (scancode = 0; scancode < 256; scancode++)
        {
            got = lookup(scancode, caps);
            expect(got == expected_char(scancode, caps), "lookup", scancode, caps, got, expected_char(scancode, caps));
        }
    }
    printf("lookup: all 256 values, caps lock off and on\n");

    // A few keys by name.
    got = lookup(0x0e, 0);
    expect(got == 0x08, "backspace", 0x0e, 0, got, 0x08);
    got = lookup(0x1c, 0);
    expect(got == 0x0d, "enter", 0x1c, 0, got, 0x0d);
    got = lookup(TABLE_SIZE - 1, 0);
    expect(got == '.', "last table entry", TABLE_SIZE - 1, 0, got, '.');
    got = lookup(TABLE_SIZE, 1);
    expect(got == 0, "first value past the table", TABLE_SIZE, 1, got, 0);
    got = lookup(0x1e, 0);
    expect(got == 'a', "a, caps lock off", 0x1e, 0, got, 'a');
    got = lookup(0x1e, 1);
    expect(got == 'A', "A, caps lock on", 0x1e, 1, got, 'A');
    printf("lookup: backspace, enter, the last entry, the first value past the table and a letter\n");

    // 2. The interrupt handler, one byte from port 0x60 at a time.
    for (caps = 0; caps <= 1; caps++)
    {
        for (scancode = 0; scancode < 256; scancode++)
        {
            keyboard_set_capslock(&classic_keyboard, caps ? KEYBOARD_CAPS_LOCK_ON : KEYBOARD_CAPS_LOCK_OFF);
            port60_value = scancode;
            port60_reads = 0;
            pushed_count = 0;
            pushed_char = -1;
            classic_keyboard_handle_interrupt();

            int toggles = scancode == CLASSIC_KEYBOARD_CAPSLOCK || scancode == CLASSIC_KEYBOARD_LSHIFT ||
                          scancode == CLASSIC_KEYBOARD_LSHIFT_R;
            int caps_after = toggles ? !caps : caps;
            int now = keyboard_get_capslock(&classic_keyboard) == KEYBOARD_CAPS_LOCK_ON;
            expect(now == caps_after, "caps lock after the interrupt", scancode, caps, now, caps_after);
            expect(port60_reads == 1, "port 0x60 reads", scancode, caps, port60_reads, 1);

            int want = scancode & CLASSIC_KEYBOARD_KEY_RELEASED ? 0 : expected_char(scancode, caps_after);
            if (want)
            {
                expect(pushed_count == 1 && pushed_char == want, "pushed key", scancode, caps,
                       pushed_count == 1 ? pushed_char : -pushed_count, want);
            }
            else
            {
                expect(pushed_count == 0, "nothing pushed", scancode, caps, pushed_count, 0);
            }
        }
    }
    printf("interrupt: all 256 values from port 0x60, caps lock off and on\n");

    printf("keyboard-scancode host check: %d of %d checks failed\n", failed, checks);
    fflush(0);
    return failed ? 1 : 0;
}
