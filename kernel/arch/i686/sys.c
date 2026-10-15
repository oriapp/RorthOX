#include "include/kernel/sys.h"
#include "include/kernel/tty.h"
#include "config.h"
#include "string/string.h"

/*
 * Implement a blue screen, with reasons and etc.
 * Please future me! MAKE IT GENERIC.
 */
char whatToDo[][138] = {
    "A problem has been detected and RorthOX has been shut down to prevent damage to your computer.\n",
    "If this is the first time you've seen this stop error screen, restart your computer. If this screen appears again, follow these steps: \n",
    "Check to make sure any new hardware or software is properly installed.\n",
    "If this a new install, check your software/hardware vendor for\n",
    "any drivers or OS updates you many need.\n",
    "If problems continue, remove any newly installed hardware or software.\n",
    "Disable BIOS memory options such as caching or shadowing.\n",
    "\n",
    "Finally, if all else failes, go to https://github.com/oriapp/RorthOX/issues\n",
    "and submit an issue with the following below information.\n",
    "\n",
    "Reason: ",
    "Error description\n.",
    "More Information\n",
};

static void panic_debug_field(const char *str)
{
    size_t i = 0;
    while (i < 1023 && str[i])
    {
        dbg_putc(str[i++]);
    }
    if (i == 1023 && str[i])
    {
        dbg_puts("...");
    }
}

static void panic_screen_field(const char *str, size_t max)
{
    size_t length = 0;
    while (length < max && str[length])
    {
        length++;
    }
    int truncated = length == max && str[length];
    if (truncated)
    {
        length -= 3;
    }
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = str[i];
        terminal_writechar(c < 32 || c == 127 ? ' ' : c, VGA_COLOR_WHITE);
    }
    if (truncated)
    {
        print("...");
    }
}

void PANIC(const char *msg, const char *file, int line)
{
    IRQ_OFF();
    if (!msg)
    {
        msg = "(no message)";
    }
    if (!file)
    {
        file = "(no file)";
    }
    char num[12];
    snprintf(num, sizeof(num), "%d", line);
#if DEBUG
    dbg_puts("\33[31m\n");
    dbg_puts("\033[1m[PANIC]: ");
    panic_debug_field(msg);
    dbg_puts(" at ");
    panic_debug_field(file);
    dbg_puts(":");
    dbg_puts(num);
    dbg_puts("\033[0m\n");
#endif
    terminal_initialize(VGA_COLOR_RED);
    for (size_t i = 0; i < 14; i++)
    {
        print(whatToDo[i]);
        if (i == 11)
        {
            panic_screen_field(msg, 71);
            print("\nat ");
            panic_screen_field(file, 64);
            print(":");
            print(num);
            print("\n");
        }
    }
    PAUSE();
}
