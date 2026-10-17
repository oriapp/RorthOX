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

    /* Mixed bytes and special keys, partly drained and refilled so the queue wraps with unread
     * keys in it, full at every refill. A shadow queue says what must come out. Zero is never
     * queued. The fields around the buffer must keep their values. */
    static int shadow[4 * RORTHOX_KEYBOARD_BUFFER_SIZE];
    int first = 0, last = 0;
    unsigned int seed = 7;
    proc.size = 0x5a5a5a5a;
    for (unsigned i = 0; i < sizeof(proc.arguments); i++)
        ((unsigned char *)&proc.arguments)[i] = 0x5a;
    for (int round = 0; round < 6; round++)
    {
        for (int dropped = 0; dropped < 3;)
        {
            seed = seed * 1103515245 + 12345;
            int k = (seed >> 16) % (nkeys + 1);
            int key = k == nkeys ? 0 : keys[k];
            if (k < nkeys && keys[k] == 'a')
                key = 1 + (seed >> 8) % 255;
            keyboard_push(key);
            if (key && last - first < size)
                shadow[last++] = key;
            else if (key)
                dropped++;
        }
        expect(proc.keyboard.tail - proc.keyboard.head == size, "full after refill", round,
               proc.keyboard.tail - proc.keyboard.head, size);
        int drain = round == 5 ? size : 300 + round * 97;
        for (int i = 0; i < drain; i++)
        {
            int c = keyboard_pop();
            expect(c == shadow[first], "mixed queue order", round * 10000 + i, c, shadow[first]);
            first++;
        }
        if (first > 2 * size)
        {
            for (int i = first; i < last; i++)
                shadow[i - first] = shadow[i];
            last -= first;
            first = 0;
        }
    }
    expect(keyboard_pop() == 0, "empty after mixed rounds", 0, 0, 0);
    keyboard_push(0);
    expect(keyboard_pop() == 0, "zero is not queued", 0, 0, 0);
    expect(proc.size == 0x5a5a5a5a, "field before the queue", 0, proc.size, 0x5a5a5a5a);
    const unsigned char *after = (const unsigned char *)&proc.arguments;
    for (unsigned i = 0; i < sizeof(proc.arguments); i++)
        expect(after[i] == 0x5a, "field after the queue", i, after[i], 0x5a);
    printf("keyboard-queue: mixed keys, partial drains and refills checked\n");

    printf(failed ? "keyboard-queue: FAIL\n" : "keyboard-queue: PASS\n");
    return failed != 0;
}
