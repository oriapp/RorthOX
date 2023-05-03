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

static  uint8_t keyboard_scan_set_one[] = {
    0x00, 0x1B, '1', '2', '3', '4', '5',
    '6', '7', '8', '9', '0', '-', '=',
    0x08, '\t',  'Q', 'W', 'E', 'R', 'T',
    'Y', 'U', 'I', 'O', 'P', '[', ']',
    0x0D, 0x00, 'A', 'S', 'D', 'F', 'G',
    'H', 'J', 'K', 'L', ';', '\'', '`', 
    0x00, '\\', 'Z', 'X', 'C', 'V', 'B',
    'N', 'M', ',', '.', '/', 0x00, '*',
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, '7', '8', '9', '-', '4', '5',
    '6', '+', '1', '2', '3', '0', '.',
};


/* New */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

static kybd_scancode regular_scancodes[] = {
  /* Numerical keys */
  {0x02, '1'}, {0x03, '2'}, {0x04, '3'}, {0x05, '4'}, {0x06, '5'}, {0x07, '6'}, {0x08, '7'}, {0x09, '8'}, {0x0A, '9'}, {0x0B, '0'},
  /* Some characters after numerical keys */
  {0x0C, '-'}, {0x0D, '='}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'q'}, {0x11, 'w'}, {0x12, 'e'}, {0x13, 'r'}, {0x14, 't'}, {0x15, 'y'}, {0x16, 'u'}, {0x17, 'i'}, {0x18, 'o'}, {0x19, 'p'}, {0x1A, '['}, {0x1B, ']'}, {0x1C, '\n'},
  {0x1E, 'a'}, {0x1F, 's'}, {0x20, 'd'}, {0x21, 'f'}, {0x22, 'g'}, {0x23, 'h'}, {0x24, 'j'}, {0x25, 'k'}, {0x26, 'l'}, {0x27, ';'}, {0x28, '\''}, {0x29, '`'},
  {0x2B, '\\'}, {0x2C, 'z'}, {0x2D, 'x'}, {0x2E, 'c'}, {0x2F, 'v'}, {0x30, 'b'}, {0x31, 'n'}, {0x32, 'm'}, {0x33, ','}, {0x34, '.'}, {0x35, '/'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};
static kybd_scancode uppercase_scancodes[] = {
  /* Numerical keys */
  {0x02, '1'}, {0x03, '2'}, {0x04, '3'}, {0x05, '4'}, {0x06, '5'}, {0x07, '6'}, {0x08, '7'}, {0x09, '8'}, {0x0A, '9'}, {0x0B, '0'},
  /* Some characters after numerical keys */
  {0x0C, '-'}, {0x0D, '='}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'Q'}, {0x11, 'W'}, {0x12, 'E'}, {0x13, 'R'}, {0x14, 'T'}, {0x15, 'Y'}, {0x16, 'U'}, {0x17, 'I'}, {0x18, 'O'}, {0x19, 'P'}, {0x1A, '['}, {0x1B, ']'}, {0x1C, '\n'},
  {0x1E, 'A'}, {0x1F, 'S'}, {0x20, 'D'}, {0x21, 'F'}, {0x22, 'G'}, {0x23, 'H'}, {0x24, 'J'}, {0x25, 'K'}, {0x26, 'L'}, {0x27, ';'}, {0x28, '\''}, {0x29, '`'},
  {0x2B, '\\'}, {0x2C, 'Z'}, {0x2D, 'X'}, {0x2E, 'C'}, {0x2F, 'V'}, {0x30, 'B'}, {0x31, 'N'}, {0x32, 'M'}, {0x33, ','}, {0x34, '.'}, {0x35, '/'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};
static kybd_scancode shift_modified_scancodes[] = {
  /* Numerical keys */
  {0x02, '!'}, {0x03, '@'}, {0x04, '#'}, {0x05, '$'}, {0x06, '%'}, {0x07, '^'}, {0x08, '&'}, {0x09, '*'}, {0x0A, '('}, {0x0B, ')'},
  /* Some characters after numerical keys */
  {0x0C, '_'}, {0x0D, '+'}, {0x0E, '\b'}, {0x0F, '\t'},
  /* Alphabet! */
  {0x10, 'Q'}, {0x11, 'W'}, {0x12, 'E'}, {0x13, 'R'}, {0x14, 'T'}, {0x15, 'Y'}, {0x16, 'U'}, {0x17, 'I'}, {0x18, 'O'}, {0x19, 'P'}, {0x1A, '{'}, {0x1B, '}'}, {0x1C, '\n'},
  {0x1E, 'A'}, {0x1F, 'S'}, {0x20, 'D'}, {0x21, 'F'}, {0x22, 'G'}, {0x23, 'H'}, {0x24, 'J'}, {0x25, 'K'}, {0x26, 'L'}, {0x27, ':'}, {0x28, '"'}, {0x29, '~'},
  {0x2B, '\\'}, {0x2C, 'Z'}, {0x2D, 'X'}, {0x2E, 'C'}, {0x2F, 'V'}, {0x30, 'B'}, {0x31, 'N'}, {0x32, 'M'}, {0x33, '<'}, {0x34, '>'}, {0x35, '?'}, {0x37, '*'},
  {0x39, ' '}, {0x47, '7'}, {0x48, '8'}, {0x49, '9'}, {0x4A, '-'},
               {0x4B, '4'}, {0x4C, '5'}, {0x4D, '6'}, {0x4E, '+'},
               {0x4F, '1'}, {0x50, '2'}, {0x51, '3'},
               {0x52, '0'}, {0x53, '.'}, {0x00, '\0'}
};

typedef struct {
    int lshift : 1;
    int rshift : 1;
    int lctrl  : 1;
    int rctrl  : 1;
    int numlk  : 1;
    int capslk : 1;
    int scrllk : 1;
} kybd_modifier_key_states;
static kybd_modifier_key_states modifier_keys;

#pragma GCC diagnostic pop

/* New */

int classic_keyboard_init();
void classic_keyboard_handle_interrupt();
void keyboard_toggle_capslock();

struct keyboard classic_keyboard = {
    .name = {"Classic"}, // Not supporting the mac one's
    .init = classic_keyboard_init,
};

int classic_keyboard_init()
{
    idt_register_interrupt_callback(ISR_KEYBOARD_INTERRUPT, classic_keyboard_handle_interrupt);

    keyboard_set_capslock(&classic_keyboard, KEYBOARD_CAPS_LOCK_OFF);

    outb(PS2_PORT, PS2_COMMAND_ENABLE_FIRST_PORT); // enable port 64
    // command 0xAE
    return 0;
}

uint8_t classic_keyboard_scancode_to_char(uint8_t scancode)
{
    size_t size_of_keyboard_set_one = sizeof(keyboard_scan_set_one) / sizeof(uint8_t);
    if (scancode > size_of_keyboard_set_one)
    {
        return 0;
    }

    char c = keyboard_scan_set_one[scancode];
    if (keyboard_get_capslock(&classic_keyboard) == KEYBOARD_CAPS_LOCK_OFF)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c += 32;
        }
    }
    return c;
}

void classic_keyboard_handle_interrupt()
{
    kernel_page();
    uint8_t scancode = 0;
    scancode = insb(KEYBOARD_INPUT_PORT);
    insb(KEYBOARD_INPUT_PORT);

    if (scancode & CLASSIC_KEYBOARD_KEY_RELEASED)
    {
        if (scancode == 0xAA) // L Shift
        {
            keyboard_toggle_capslock();
        }
        // print(itoa(scancode));
        return;
    }

    switch (scancode)
    {
    case CLASSIC_KEYBOARD_CAPSLOCK:
        keyboard_toggle_capslock();
        break;
    case CLASSIC_KEYBOARD_LSHIFT:
        // toggle special marks (! ? | and etc...)
        keyboard_toggle_capslock();
        break;
    
    default:
        break;
    }

    uint8_t c = classic_keyboard_scancode_to_char(scancode);
    if (c != 0) // valid scancode
    {
        keyboard_push(c);
    }
    task_page();
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
