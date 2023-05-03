#include "include/kernel/sys.h"
#include "include/kernel/tty.h"
#include "config.h"
#include "string/string.h"

/*
 * Implement a blue screen, with reasons and etc.
 * Please future me! MAKE IT GENERIC.
 */
char whatToDo[][138] = {
    "A problem has been detected and RorthOS has been shut down to prevent damage to your computer.\n",
    "If this is the first time you've seen this stop error screen, restart your computer. If this screen appears again, follow these steps: \n",
    "Check to make sure any new hardware or software is properly installed.\n",
    "If this a new install, check your software/hardware vendor for\n",
    "any drivers or OS updates you many need.\n",
    "If problems continue, remove any newly installed hardware or software.\n",
    "Disable BIOS memory options such as caching or shadowing.\n",
    "\n",
    "Finally, if all else failes, go to https://github.com/oriapp/RorthOSX/issues\n",
    "and submit an issue with the following below information.\n",
    "\n",
    "Reason: Reason!\n",
    "Error description\n.",
    "More Information\n",
};

void PANIC(const char *msg, const char *file, int line)
{
#if DEBUG
    if (strlen(msg) < 1000) // 24 addition symbols
    {
        // set_task(0);
        char linerr[1024] = "[PANIC]: ";
        strcat(linerr, msg);
        strcat(linerr, " at ");
        strcat(linerr, file);
        strcat(linerr, ":");
        strcat(linerr, itoa(line));
        dbg_puts("\33[31m\n");
        dbg_puts("\033[1m");
        dbg_puts(linerr);
        dbg_puts("\033[0m");
        dbg_puts("\n");
    }
#endif
    terminal_initialize(VGA_COLOR_RED);
    // disable cursor
    for (size_t i = 0; i < 14; i++)
    {
        print(whatToDo[i]);
    }
    // print(msg);
    // IRQ_OFF();
    PAUSE();
}
