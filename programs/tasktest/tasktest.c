/*
 * Task list test for scripts/tests/task-list.sh (only copied into the test image).
 *
 * "tasktest.elf" (the parent) starts "tasktest.elf child" with syscall 7 and then exits while
 * the child is still running, so the parent's task is taken out of the middle of the task list
 * (shell, parent, child). The child waits for a key before it exits, so the script can check
 * that the parent is gone first. Afterwards the shell must still start programs.
 *
 * One line per print call: the shell runs alongside and may print between two calls.
 * No globals: the loader doesn't map .bss.
 */
#include "rorthox.h"
#include "string.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strncmp(argv[1], "child", 6) == 0)
    {
        print("tasktest child: ready\n");
        rorthox_getkeyblock();
        print("tasktest child: done\n");
        return 0;
    }

    print("tasktest parent: start\n");
    rorthox_system_run("tasktest.elf child");
    print("tasktest parent: exiting\n");
    return 0;
}
