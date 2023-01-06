#include "include/kernel/sys.h"
#include "include/kernel/tty.h"

void PANIC(const char *msg)
{
    print(msg);
    PAUSE();
}
