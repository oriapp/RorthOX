#include "potongos.h"
#define UNUSED(x) (void)(x)

extern int main(int argc, char **argv);

void c_start()
{
    struct process_arguments arguments;
    potongos_process_get_arguments(&arguments);
    

    // Res could be used for exit command or sld
    int res = main(arguments.argc, arguments.argv);
    UNUSED(res);
}