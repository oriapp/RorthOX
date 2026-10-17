#include "classic.h"
#include "keyboard.h"
#include "io/io.h"
#include "kernel.h"
#include "idt/idt.h"
#include "task/task.h"
#include <stdint.h>
#include <stddef.h>
#include "string/string.h"

#define CLASSIC_KEYBOARD_CAPSLOCK   0x3A
#define CLASSIC_KEYBOARD_LSHIFT     0x2A
#define CLASSIC_KEYBOARD_LSHIFT_R   0xAA
#define CLASSIC_KEYBOARD_RSHIFT     0x36
#define CLASSIC_KEYBOARD_RSHIFT_R   0xB6
#define CLASSIC_KEYBOARD_NUMLOCK    0x45
#define CLASSIC_KEYBOARD_NUMLOCK_R  0xC5
#define CLASSIC_KEYBOARD_EXTENDED   0xE0
#define CLASSIC_KEYBOARD_PAUSE      0xE1

// Pause sends E1 1D 45 E1 9D C5 and no break. Each E1 drops the two bytes after it, so the 45
// inside never toggles Num Lock and a cut-off sequence loses at most two bytes.
#define CLASSIC_KEYBOARD_PAUSE_PAYLOAD 2

// The keypad sends the same make codes whatever the Num Lock state (47-53, no prefix); the driver
// keeps Num Lock itself. With Num Lock off the digits and the decimal point give nothing, while
// - and + (4A, 4E), * (37), / and Enter (E0 35, E0 1C) always work. Shift doesn't change this.
static const kybd_scancode regular_scancodes[] = {
  {0x01, 0x1B},
  /* Numerical keys */
  {0x02, '1'}, {0x03, '2'}, {0x04, '3'}, {0x05, '4'}, {0x06, '5'}, {0x07, '6'}, {0x08, '7'}, {0x09, '8'}, {0x0A, '9'}, {0x0B, '0'},
  /* Some characters after numerical keys */
  {0x0C, '-'}, {0x0D, '='}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'q'}, {0x11, 'w'}, {0x12, 'e'}, {0x13, 'r'}, {0x14, 't'}, {0x15, 'y'}, {0x16, 'u'}, {0x17, 'i'}, {0x18, 'o'}, {0x19, 'p'}, {0x1A, '['}, {0x1B, ']'}, {0x1C, 0x0D},
  {0x1E, 'a'}, {0x1F, 's'}, {0x20, 'd'}, {0x21, 'f'}, {0x22, 'g'}, {0x23, 'h'}, {0x24, 'j'}, {0x25, 'k'}, {0x26, 'l'}, {0x27, ';'}, {0x28, '\''}, {0x29, '`'},
  {0x2B, '\\'}, {0x2C, 'z'}, {0x2D, 'x'}, {0x2E, 'c'}, {0x2F, 'v'}, {0x30, 'b'}, {0x31, 'n'}, {0x32, 'm'}, {0x33, ','}, {0x34, '.'}, {0x35, '/'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};
static const kybd_scancode uppercase_scancodes[] = {
  {0x01, 0x1B},
  /* Numerical keys */
  {0x02, '1'}, {0x03, '2'}, {0x04, '3'}, {0x05, '4'}, {0x06, '5'}, {0x07, '6'}, {0x08, '7'}, {0x09, '8'}, {0x0A, '9'}, {0x0B, '0'},
  /* Some characters after numerical keys */
  {0x0C, '-'}, {0x0D, '='}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'Q'}, {0x11, 'W'}, {0x12, 'E'}, {0x13, 'R'}, {0x14, 'T'}, {0x15, 'Y'}, {0x16, 'U'}, {0x17, 'I'}, {0x18, 'O'}, {0x19, 'P'}, {0x1A, '['}, {0x1B, ']'}, {0x1C, 0x0D},
  {0x1E, 'A'}, {0x1F, 'S'}, {0x20, 'D'}, {0x21, 'F'}, {0x22, 'G'}, {0x23, 'H'}, {0x24, 'J'}, {0x25, 'K'}, {0x26, 'L'}, {0x27, ';'}, {0x28, '\''}, {0x29, '`'},
  {0x2B, '\\'}, {0x2C, 'Z'}, {0x2D, 'X'}, {0x2E, 'C'}, {0x2F, 'V'}, {0x30, 'B'}, {0x31, 'N'}, {0x32, 'M'}, {0x33, ','}, {0x34, '.'}, {0x35, '/'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};
static const kybd_scancode shift_modified_scancodes[] = {
  {0x01, 0x1B},
  /* Numerical keys */
  {0x02, '!'}, {0x03, '@'}, {0x04, '#'}, {0x05, '$'}, {0x06, '%'}, {0x07, '^'}, {0x08, '&'}, {0x09, '*'}, {0x0A, '('}, {0x0B, ')'},
  /* Some characters after numerical keys */
  {0x0C, '_'}, {0x0D, '+'}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'Q'}, {0x11, 'W'}, {0x12, 'E'}, {0x13, 'R'}, {0x14, 'T'}, {0x15, 'Y'}, {0x16, 'U'}, {0x17, 'I'}, {0x18, 'O'}, {0x19, 'P'}, {0x1A, '{'}, {0x1B, '}'}, {0x1C, 0x0D},
  {0x1E, 'A'}, {0x1F, 'S'}, {0x20, 'D'}, {0x21, 'F'}, {0x22, 'G'}, {0x23, 'H'}, {0x24, 'J'}, {0x25, 'K'}, {0x26, 'L'}, {0x27, ':'}, {0x28, '"'}, {0x29, '~'},
  {0x2B, '|'}, {0x2C, 'Z'}, {0x2D, 'X'}, {0x2E, 'C'}, {0x2F, 'V'}, {0x30, 'B'}, {0x31, 'N'}, {0x32, 'M'}, {0x33, '<'}, {0x34, '>'}, {0x35, '?'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};

typedef struct {
    unsigned int lshift : 1;
    unsigned int rshift : 1;
} kybd_modifier_key_states;
static kybd_modifier_key_states modifier_keys;
static int extended;
static int pause_bytes_left;

// Num Lock is on at boot, whatever the firmware left behind. The LED isn't updated (no 0xED command).
static int numlock;
static int numlock_down;

// Keys that only exist with the E0 prefix. The keypad keys with the same second byte are separate keys.
typedef struct {
    uint8_t scancode;
    uint16_t key;
} kybd_extended_scancode;
static const kybd_extended_scancode extended_scancodes[] = {
  {0x1C, 0x0D}, {0x35, '/'},
  {0x48, RORTHOX_KEY_UP}, {0x50, RORTHOX_KEY_DOWN}, {0x4B, RORTHOX_KEY_LEFT}, {0x4D, RORTHOX_KEY_RIGHT},
  {0x47, RORTHOX_KEY_HOME}, {0x4F, RORTHOX_KEY_END}, {0x52, RORTHOX_KEY_INSERT}, {0x53, RORTHOX_KEY_DELETE},
  {0x49, RORTHOX_KEY_PAGE_UP}, {0x51, RORTHOX_KEY_PAGE_DOWN},
  {0x00, 0}
};

int classic_keyboard_init();
void classic_keyboard_handle_interrupt(struct interrupt_frame *frame);
void keyboard_toggle_capslock();

struct keyboard classic_keyboard = {
    .name = {"Classic"}, // Not supporting the mac one's
    .init = classic_keyboard_init,
};

int classic_keyboard_init()
{
    idt_register_interrupt_callback(ISR_KEYBOARD_INTERRUPT, classic_keyboard_handle_interrupt);

    keyboard_set_capslock(&classic_keyboard, KEYBOARD_CAPS_LOCK_OFF);
    modifier_keys.lshift = 0;
    modifier_keys.rshift = 0;
    extended = 0;
    pause_bytes_left = 0;
    numlock = 1;
    numlock_down = 0;

    outb(PS2_PORT, PS2_COMMAND_ENABLE_FIRST_PORT); // enable port 64
    // command 0xAE
    return 0;
}

uint8_t classic_keyboard_scancode_to_char(uint8_t scancode)
{
    int shift = modifier_keys.lshift || modifier_keys.rshift;
    int caps = keyboard_get_capslock(&classic_keyboard) == KEYBOARD_CAPS_LOCK_ON;
    const kybd_scancode *table = shift ? shift_modified_scancodes :
                                       (caps ? uppercase_scancodes : regular_scancodes);
    for (int i = 0; table[i].scancode != 0; i++)
    {
        if (table[i].scancode == scancode)
        {
            uint8_t c = table[i].chr;
            if (shift && caps && c >= 'A' && c <= 'Z')
            {
                c += 'a' - 'A';
            }
            return c;
        }
    }
    return 0;
}

// Keypad 7 8 9 4 5 6 1 2 3 0 . (without the prefix): these follow Num Lock
static int classic_keyboard_is_keypad_digit(uint8_t scancode)
{
    return scancode >= 0x47 && scancode <= 0x53 && scancode != 0x4A && scancode != 0x4E;
}

// The byte after E0. It never reaches the modifier keys, so fake shifts change no state and only
// makes of the keys in extended_scancodes give a key.
static void classic_keyboard_handle_extended(uint8_t scancode)
{
    if (scancode & CLASSIC_KEYBOARD_KEY_RELEASED)
    {
        return;
    }

    for (int i = 0; extended_scancodes[i].scancode != 0; i++)
    {
        if (extended_scancodes[i].scancode == scancode)
        {
            keyboard_push(extended_scancodes[i].key);
            return;
        }
    }
}

// IRQ1 callback. interrupt_handler owns the page directory, segments and EOI on both ends.
void classic_keyboard_handle_interrupt(struct interrupt_frame *frame)
{
    // One byte per interrupt: a second read here would take the next key's byte and lose it
    uint8_t scancode = insb(KEYBOARD_INPUT_PORT);

    if (pause_bytes_left > 0)
    {
        pause_bytes_left--;
        return;
    }
    if (extended)
    {
        // Exactly one byte after E0, even if it is E0 or E1 itself
        extended = 0;
        classic_keyboard_handle_extended(scancode);
        return;
    }
    if (scancode == CLASSIC_KEYBOARD_EXTENDED)
    {
        extended = 1;
        return;
    }
    if (scancode == CLASSIC_KEYBOARD_PAUSE)
    {
        pause_bytes_left = CLASSIC_KEYBOARD_PAUSE_PAYLOAD;
        return;
    }

    switch (scancode)
    {
    case CLASSIC_KEYBOARD_NUMLOCK:
        // Held keys repeat their make code: toggle only on the first one
        if (!numlock_down)
        {
            numlock = !numlock;
        }
        numlock_down = 1;
        break;
    case CLASSIC_KEYBOARD_NUMLOCK_R:
        numlock_down = 0;
        break;
    case CLASSIC_KEYBOARD_CAPSLOCK:
        keyboard_toggle_capslock();
        break;
    case CLASSIC_KEYBOARD_LSHIFT:
        modifier_keys.lshift = 1;
        break;
    case CLASSIC_KEYBOARD_LSHIFT_R:
        modifier_keys.lshift = 0;
        break;
    case CLASSIC_KEYBOARD_RSHIFT:
        modifier_keys.rshift = 1;
        break;
    case CLASSIC_KEYBOARD_RSHIFT_R:
        modifier_keys.rshift = 0;
        break;
    default:
        break;
    }

    // With Num Lock off the keypad digits and the decimal point give nothing
    int suppressed = !numlock && classic_keyboard_is_keypad_digit(scancode);
    if (!(scancode & CLASSIC_KEYBOARD_KEY_RELEASED) && !suppressed)
    {
        uint8_t c = classic_keyboard_scancode_to_char(scancode);
        if (c != 0) // valid scancode
        {
            keyboard_push(c);
        }
    }
}

void keyboard_toggle_capslock()
{
    KEYBOARD_CAPS_LOCK_STATE old_state = keyboard_get_capslock(&classic_keyboard);
    keyboard_set_capslock(&classic_keyboard, old_state == KEYBOARD_CAPS_LOCK_ON ? KEYBOARD_CAPS_LOCK_OFF : KEYBOARD_CAPS_LOCK_ON);
}

struct keyboard *classic_init()
{
    return &classic_keyboard; // the address
}
