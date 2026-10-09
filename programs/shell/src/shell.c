#include "shell.h"
#include "stdio.h"
#include "stdlib.h"
#include "rorthox.h"

int main(int argc, char const *argv[])
{
    print("Codename Spectrum\n");
    print("RorthOX v1.0.0\n");
    while (1) {
        print("> ");
        char buf[1024];
        rorthox_terminal_readline(buf, sizeof(buf), true);

        print("\n");
        
        rorthox_system_run(buf);
    }
    return 0;
}
