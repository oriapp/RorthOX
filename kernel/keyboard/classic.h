#ifndef CLASSIC_KEYBOARD_H
#define CLASSIC_KEYBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FUTURE USE
typedef struct {
    uint8_t scancode;
    char chr; // Character it corresponds to.
} kybd_scancode;

#define PS2_PORT 0x64
#define PS2_COMMAND_ENABLE_FIRST_PORT 0xAE

#define CLASSIC_KEYBOARD_KEY_RELEASED 0x80
#define ISR_KEYBOARD_INTERRUPT 0x21
#define KEYBOARD_INPUT_PORT 0x60

struct keyboard *classic_init();

#ifdef __cplusplus
}
#endif

#endif // !CLASSIC_KEYBOARD_H