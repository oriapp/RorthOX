#include "shell.h"
#include "stdio.h"
#include "stdlib.h"
#include "potongos.h"

int main(int argc, char const *argv[])
{
    print("Codename Spectrum\n");
    print("Rorth v1.0.0\n");
    while (1) {
        print("> ");
        char buf[1024];
        potongos_terminal_readline(buf, sizeof(buf), true);

        print("\n");
        
        potongos_system_run(buf);
    }
    return 0;
}
