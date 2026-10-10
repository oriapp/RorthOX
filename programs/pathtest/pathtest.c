/*
 * Program name length test for scripts/tests/syscall-path.sh (only copied into the test image).
 *
 * The kernel builds "0:/" + name in a RORTHOX_MAX_PATH byte buffer. A name that doesn't fit
 * (more than RORTHOX_MAX_PATH - 4 characters) must be refused before anything is written, and
 * must never start a program. The names here are "blank.elf" followed by slashes: the path
 * parser stops at the first empty part, so such a name opens blank.elf when the kernel accepts it.
 *
 * Syscall 7 (start a program with arguments) must return -EINVARG for every name that is too
 * long. A name that fits but names no file must still reach the loader and fail there (some other
 * error). Syscall 6 (start a program) returns nothing, so for it the test script checks that
 * blank.elf never printed anything.
 *
 * Prints "pathtest: FAIL ..." lines, then "pathtest: PASS" if every check passed, then
 * "pathtest: done". No globals: the loader doesn't map .bss.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "config.h"
#include "status.h"

// Longest name that fits after "0:/" with its terminator.
#define FITS (RORTHOX_MAX_PATH - 4)

_Static_assert(RORTHOX_COMMAND_ARGUMENT_SIZE > 300, "pathtest uses names of up to 511 characters");

// One line per print call: the shell runs alongside and may print between two calls.
static void fail(const char *what, int length, int result, int expected)
{
    char line[120];
    strcpy(line, "pathtest: FAIL ");
    strcpy(line + strlen(line), what);
    strcpy(line + strlen(line), " length ");
    strcpy(line + strlen(line), itoa(length));
    strcpy(line + strlen(line), " result ");
    strcpy(line + strlen(line), itoa(result));
    strcpy(line + strlen(line), " expected ");
    strcpy(line + strlen(line), itoa(expected));
    strcpy(line + strlen(line), "\n");
    print(line);
}

// Fill out with <length> characters: <start> and then <pad> up to the length.
static void make_name(char *out, int length, const char *start, char pad)
{
    int i = 0;
    for (; i < length && start[i]; i++)
    {
        out[i] = start[i];
    }
    for (; i < length; i++)
    {
        out[i] = pad;
    }
    out[i] = 0;
}

int main(int argc, char **argv)
{
    // Too long by 1, 2, 3 characters (what used to land past the buffer), then much longer.
    const int too_long[] = { FITS + 1, FITS + 2, FITS + 3, FITS + 4, 300, RORTHOX_COMMAND_ARGUMENT_SIZE - 1 };
    const int count = sizeof(too_long) / sizeof(too_long[0]);
    int failed = 0;

    struct command_argument *command = malloc(sizeof(struct command_argument));
    if (!command)
    {
        print("pathtest: FAIL no memory\n");
        print("pathtest: done\n");
        return 1;
    }

    // Syscall 7, names that don't fit: refused.
    for (int i = 0; i < count; i++)
    {
        make_name(command->argument, too_long[i], "blank.elf", '/');
        command->next = 0;
        int res = rorthox_system(command);
        if (res != -EINVARG)
        {
            fail("syscall 7 blank.elf+slashes", too_long[i], res, -EINVARG);
            failed = 1;
        }
    }

    // Syscall 7, a name that just fits but names no file: the loader fails, not the length check.
    make_name(command->argument, FITS, "nofile", 'x');
    command->next = 0;
    int res = rorthox_system(command);
    if (res >= 0 || res == -EINVARG)
    {
        fail("syscall 7 nofile", FITS, res, -EIO);
        failed = 1;
    }

    // Syscall 6, names that don't fit (it truncates to RORTHOX_MAX_PATH - 1 characters first).
    for (int i = 0; i < count; i++)
    {
        make_name(command->argument, too_long[i], "blank.elf", '/');
        rorthox_process_load_start(command->argument);
    }

    free(command);
    if (!failed)
    {
        print("pathtest: PASS\n");
    }
    print("pathtest: done\n");
    return failed;
}
