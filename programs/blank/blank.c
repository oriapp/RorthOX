#include "potongos.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char const **argv)
{

    print("Blank loaded\n");

    int i = 0;
    for (i = 0; i < argc; i++)
    {
        printf("%s\n", argv[i]);
    }

    // char str[] = "Hello World!";
    // struct command_argument *root_command =  potongos_parse_command(str, sizeof(str));
    // printf("%s\n", root_command->argument);
    // printf("%s\n", root_command->next->argument);

    return 0;
}
