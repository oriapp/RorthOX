#include "shell.h"
#include "rorthox-config.h"
#include "stdio.h"
#include "stdlib.h"
#include "rorthox.h"

int main(int argc, char const *argv[])
{
    print("Codename Spectrum\n");
    print(RORTHOX_NAME " v" RORTHOX_VERSION "\n");
    while (1) {
        print("> ");
        char buf[1024];
        rorthox_terminal_readline(buf, sizeof(buf), true);

        print("\n");

        int words = rorthox_command_word_count(buf);
        if (words > RORTHOX_MAX_COMMAND_ARGUMENTS)
        {
            printf("Too many words (maximum %i).\n", RORTHOX_MAX_COMMAND_ARGUMENTS);
            continue;
        }
        if (words > 0)
        {
            rorthox_system_run(buf);
        }
    }
    return 0;
}
