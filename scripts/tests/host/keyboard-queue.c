/* The per-process key queue: a full queue drops new keys and never overwrites unread ones,
 * keys come out in order, and the queue keeps working after it wraps around. Entries are
 * 16-bit keys: the capacity counts keys, and RORTHOX_KEY_* values come out whole. */
#include "keyboard/keyboard.c"
#include "host.h"

static struct process proc;
static struct task tsk;
struct process *process_current() { return &proc; }
struct task *task_current() { return &tsk; }
struct keyboard *classic_init() { return 0; }

static int failed;
static void expect(int ok, const char *what, int i, int got, int want)
{
    if (!ok && ++failed <= 10)
        printf("FAIL %s: i=%d got=%d want=%d\n", what, i, got, want);
}

int main(void)
{
    const int size = sizeof(proc.keyboard.buffer) / sizeof(proc.keyboard.buffer[0]);
    tsk.process = &proc;
    expect(size == RORTHOX_KEYBOARD_BUFFER_SIZE, "capacity in keys", 0, size, RORTHOX_KEYBOARD_BUFFER_SIZE);

    /* Bytes above 0x7f and the special keys keep their full value. */
    const int keys[] = {'a', 0x80, 0xff, RORTHOX_KEY_UP, RORTHOX_KEY_DOWN, RORTHOX_KEY_LEFT, RORTHOX_KEY_RIGHT,
                        RORTHOX_KEY_HOME, RORTHOX_KEY_END, RORTHOX_KEY_INSERT, RORTHOX_KEY_DELETE,
                        RORTHOX_KEY_PAGE_UP, RORTHOX_KEY_PAGE_DOWN};
    const int nkeys = sizeof(keys) / sizeof(keys[0]);
    for (int i = 0; i < nkeys; i++)
        keyboard_push(keys[i]);
    for (int i = 0; i < nkeys; i++)
    {
        int c = keyboard_pop();
        expect(c == keys[i], "full key value", i, c, keys[i]);
    }
    expect(keyboard_pop() == 0, "empty after key values", nkeys, 0, 0);

    for (int i = 0; i < size; i++)
        keyboard_push('a' + i % 26);
    keyboard_push('X');                 /* one more than fits: must be dropped */
    for (int i = 0; i < size; i++)
    {
        int c = keyboard_pop();
        expect(c == 'a' + i % 26, "full queue order", i, c, 'a' + i % 26);
    }
    expect(keyboard_pop() == 0, "empty after draining", size, 0, 0);

    /* Wrap around: half in, half out, then fill to the limit again. */
    for (int round = 0; round < 3; round++)
    {
        for (int i = 0; i < size; i++)
            keyboard_push('0' + i % 10);
        keyboard_push('Y');
        for (int i = 0; i < size; i++)
        {
            int c = keyboard_pop();
            expect(c == '0' + i % 10, "wrapped queue order", i, c, '0' + i % 10);
        }
        expect(keyboard_pop() == 0, "empty after wrap", round, 0, 0);
    }

    printf(failed ? "keyboard-queue: FAIL\n" : "keyboard-queue: PASS\n");
    return failed != 0;
}
