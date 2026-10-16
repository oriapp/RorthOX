/* The per-process key queue: a full queue drops new keys and never overwrites unread ones,
 * keys come out in order, and the queue keeps working after it wraps around. */
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
    const int size = sizeof(proc.keyboard.buffer);
    tsk.process = &proc;

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
