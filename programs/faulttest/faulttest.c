/*
 * Exception test for scripts/tests/exceptions.sh and kernel-exceptions.sh (only copied into
 * the test images, as fault.elf: the FAT16 driver has no long names).
 *
 *   fault.elf ud2|int3|cli|pf|int14|int18
 *                                     prints "faulttest: armed", waits for a key, then raises
 *                                     the CPU exception named by the argument (pf: allocate a
 *                                     page, free it and read it). The kernel must end the
 *                                     program, so "faulttest: survived" must never show.
 *   fault.elf kcopy                   the same, but passes a freed (unmapped) pointer to print,
 *                                     so the page fault happens in the kernel's copy.
 *   fault.elf parent                  starts "fault.elf child" (which then owns the keyboard)
 *                                     and raises #UD itself.
 *   fault.elf child                   prints "faulttest child: ready", waits for a key, prints
 *                                     "faulttest child: done" and exits.
 *
 * One line per print call: the shell runs alongside and may print between two calls.
 * No globals: the loader doesn't map .bss.
 */
#include <stdint.h>
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

void fault_ud2(void);
void fault_int3(void);
void fault_cli(void);
uint32_t fault_read(void *ptr);
void fault_int14(void);
void fault_int18(void);

static int is(const char *arg, const char *name)
{
    return strncmp(arg, name, strlen(name) + 1) == 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        print("faulttest: usage: fault.elf ud2|int3|cli|pf|int14|int18|kcopy|parent|child\n");
        return 1;
    }

    const char *mode = argv[1];
    if (is(mode, "child"))
    {
        print("faulttest child: ready\n");
        rorthox_getkeyblock();
        print("faulttest child: done\n");
        return 0;
    }

    if (is(mode, "parent"))
    {
        rorthox_system_run("fault.elf child");
        print("faulttest parent: faulting\n");
        fault_ud2();
        print("faulttest: survived\n");
        return 0;
    }

    // An allocation the program still holds when it faults: ending the program must free it.
    void *held = malloc(64);
    print("faulttest: armed\n");
    rorthox_getkeyblock();

    if (is(mode, "ud2"))
    {
        fault_ud2();
    }
    else if (is(mode, "int3"))
    {
        fault_int3();
    }
    else if (is(mode, "cli"))
    {
        fault_cli();
    }
    else if (is(mode, "int14"))
    {
        fault_int14();
    }
    else if (is(mode, "int18"))
    {
        fault_int18();
    }
    else if (is(mode, "pf"))
    {
        // Nothing may allocate between the free and the read.
        void *page = malloc(4096);
        free(page);
        fault_read(page);
    }
    else if (is(mode, "kcopy"))
    {
        // Two pages: print's own kernel buffer may reuse the first one, never the second.
        char *pages = malloc(8192);
        free(pages);
        print(pages + 4096);
    }
    else
    {
        print("faulttest: unknown mode\n");
        free(held);
        return 1;
    }

    print("faulttest: survived\n");
    free(held);
    return 0;
}
