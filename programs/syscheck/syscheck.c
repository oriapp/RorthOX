#include "rorthox.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "memory.h"

/* Observe EAX without changing the public void wrapper. */
int get_arguments(struct process_arguments *) __asm__("rorthox_process_get_arguments");
/* Observe EAX of the string syscalls (1, 6, 10) through the same wrappers. */
int print_result(const char *) __asm__("print");
int printc_result(const char *, int) __asm__("printc");
int load_result(const char *) __asm__("rorthox_process_load_start");

static void armed(void)
{
    print("syscheck: armed\n");
    rorthox_getkeyblock();
}

static struct process_arguments writable = {123, (void *)456};
static struct process_arguments zeroed;
static const unsigned char readonly[16] = "do not overwrite";
int bad_stack(int command, unsigned int sp);

static int same(const volatile unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int arguments_ok(struct process_arguments *p, int argc, char **argv)
{
    return get_arguments(p) == 0 && p->argc == argc && p->argv == argv;
}
static int arguments_test(int argc, char **argv)
{
    struct process_arguments stack;
    unsigned char *heap = malloc(8192);
    if (!heap) return 1;
    int bad = !arguments_ok(&stack, argc, argv);
    bad |= !arguments_ok((void *)(heap + 3), argc, argv);
    bad |= !arguments_ok((void *)(heap + 4093), argc, argv);
    bad |= !arguments_ok(&writable, argc, argv);
    bad |= !arguments_ok(&zeroed, argc, argv);
    unsigned char before[16];
    for (int i = 0; i < 16; i++) before[i] = ((volatile unsigned char *)readonly)[i];
    int ro = get_arguments((void *)readonly);
    int unchanged = same(readonly, before, 16);
    printf("syscheck: readonly %i unchanged %i\n", ro, unchanged);
    bad |= ro != -2 || !unchanged;
    volatile unsigned char *outside = (void *)0x800000;
    for (int i = 0; i < 16; i++) before[i] = outside[i];
    int unowned = get_arguments((void *)outside);
    unchanged = same(outside, before, 16);
    printf("syscheck: outside %i unchanged %i\n", unowned, unchanged);
    bad |= unowned != -2 || !unchanged;
    /* Only four of eight bytes are owned: refusal must not store argc first. */
    unsigned char *tiny = malloc(4);
    if (!tiny) return 1;
    memset(tiny, 0x5a, 4);
    bad |= get_arguments((void *)tiny) != -2;
    for (int i = 0; i < 4; i++) bad |= tiny[i] != 0x5a;
    bad |= !arguments_ok(&stack, argc, argv);
    free(tiny); free(heap);
    return bad;
}

static int freed_arguments_test(const char *mode, int argc, char **argv)
{
    unsigned char *memory = malloc(8192);
    struct process_arguments stack;
    if (!memory) return 1;
    /* Keep the operand in the second page, away from one-block print buffers. */
    unsigned int address = (unsigned int)(memory + 4096);
    *(unsigned int *)(memory + 4096) = (unsigned int)&stack;
    free(memory);
    int rc = !strncmp(mode, "freesp", 7) ?
        bad_stack(SYSCALL_GET_PROGRAM_ARGUMENTS, address) : get_arguments((void *)address);
    printf("syscheck: freed result %i\n", rc);
    return rc != -2 || !arguments_ok(&stack, argc, argv);
}

static struct command_argument data_node = { "child", 0 };
static const struct command_argument read_node = { "child", 0 };

static int rejected(struct command_argument *root)
{
    int rc = rorthox_system(root);
    if (rc != -2) printf("syscheck: bad result %i\n", rc);
    return rc != -2;
}
static int command_test(const char *mode)
{
    if (!strncmp(mode, "free", 4))
    {
        unsigned char *memory = malloc(8192);
        struct command_argument root = { "syscheck.elf", 0 };
        /* A live block after these two pages keeps the snapshot from reusing them. */
        void *guard = malloc(4096);
        if (!memory || !guard) return 1;
        unsigned int address = (unsigned int)(memory + 4096);
        struct command_argument *stale = (void *)address;
        memset(stale, 0, sizeof(*stale));
        strcpy(stale->argument, !strncmp(mode, "freenext", 9) ? "child" : "syscheck.elf");
        if (!strncmp(mode, "freesp", 7)) *(unsigned int *)stale = (unsigned int)&root;
        free(memory);
        int rc;
        if (!strncmp(mode, "freesp", 7)) rc = bad_stack(SYSCALL_INVOKE_SYSTEM_COMMAND, address);
        else
        {
            if (!strncmp(mode, "freenext", 9)) root.next = stale;
            rc = rorthox_system(root.next ? &root : stale);
        }
        printf("syscheck: freed result %i\n", rc);
        print(rc != -2 ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: key\n"); rorthox_getkeyblock();
        free(guard);
        return rc != -2;
    }
    unsigned char *memory = malloc((RORTHOX_MAX_COMMAND_ARGUMENTS+1)*sizeof(struct command_argument)+8192);
    if (!memory) return 1;
    struct command_argument *nodes = (void *)memory;
    memset(memory, 0, (RORTHOX_MAX_COMMAND_ARGUMENTS+1)*sizeof(*nodes)+8192);
    strcpy(nodes[0].argument, "syscheck.elf");
    if (!strncmp(mode,"bad",4))
    {
        int bad=0;
        bad |= rejected(0);
        bad |= rejected((void *)0x800000);
        bad |= rejected((void *)0xffffff00U);
        nodes[0].argument[0]=0; bad |= rejected(nodes);
        memset(nodes[0].argument,'x',sizeof(nodes[0].argument)); bad |= rejected(nodes);
        strcpy(nodes[0].argument,"syscheck.elf");
        nodes[0].next=(void *)0x800000; bad |= rejected(nodes);
        nodes[0].next=&nodes[1];
        memset(nodes[1].argument,'x',sizeof(nodes[1].argument)); bad |= rejected(nodes);
        memset(nodes[1].argument,0,sizeof(nodes[1].argument));
        for(int i=0;i<RORTHOX_MAX_COMMAND_ARGUMENTS+1;i++)
        {
            if(i) strcpy(nodes[i].argument,"child");
            nodes[i].next=i<RORTHOX_MAX_COMMAND_ARGUMENTS?&nodes[i+1]:0;
        }
        bad |= rejected(nodes);
        unsigned char *short_memory=malloc(4093+sizeof(struct command_argument)-1);
        if (!short_memory) return 1;
        struct command_argument *short_node=(void *)(short_memory+4093);
        memset(short_node,0,sizeof(*short_node)-1); strcpy(short_node->argument,"syscheck.elf");
        bad |= rejected(short_node); free(short_memory);
        print(bad ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: key\n"); rorthox_getkeyblock();
        free(memory); return bad;
    }
    if (!strncmp(mode,"self",5) || !strncmp(mode,"two",4))
    {
        nodes[0].next = mode[0]=='s'?nodes:&nodes[1];
        strcpy(nodes[1].argument,"child"); nodes[1].next=nodes;
        int bad=rejected(nodes);
        print(bad ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: key\n"); rorthox_getkeyblock();
        free(memory); return bad;
    }
    if (!strncmp(mode,"badsp7",7))
    {
        int bad=bad_stack(SYSCALL_INVOKE_SYSTEM_COMMAND,0xfffffffeU)!=-2;
        print(bad ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        free(memory); return bad;
    }
    struct command_argument stack_node={"child",0};
    struct command_argument *root=nodes;
    if(!strncmp(mode,"mixed",6)) { nodes[0].next=&stack_node; stack_node.next=&data_node; }
    if(!strncmp(mode,"readnode",9)) nodes[0].next=(void *)&read_node;
    if(!strncmp(mode,"max",4))
    {
        for(int i=1;i<RORTHOX_MAX_COMMAND_ARGUMENTS;i++) strcpy(nodes[i].argument,i==RORTHOX_MAX_COMMAND_ARGUMENTS-1?"last":"child");
        for(int i=0;i<RORTHOX_MAX_COMMAND_ARGUMENTS-1;i++) nodes[i].next=&nodes[i+1];
    }
    if(!strncmp(mode,"boundary",9))
    {
        root=(void *)(memory+4093); memset(root,0,sizeof(*root)); strcpy(root->argument,"syscheck.elf");
        root->next=&nodes[1];
        memset(nodes[1].argument,'x',sizeof(nodes[1].argument));
        nodes[1].argument[sizeof(nodes[1].argument)-1]=0;
    }
    rorthox_system(root);
    free(memory); return 0;
}

/* Syscalls 1, 6 and 10 read their string through a checked copy: freed, unowned and
   unterminated-at-the-end pointers give an error instead of a kernel fault or an overread. */
static int pointer_test(void)
{
    int bad = 0;
    char *freed = malloc(4096);
    char *end = malloc(4096);
    if (!freed || !end) return 1;
    strcpy(freed, "syscheck: stale\n");
    free(freed);
    int rc = print_result(freed);
    printf("syscheck: freed print %i\n", rc);
    bad |= rc >= 0;
    rc = print_result((void *)0x800000);
    printf("syscheck: outside print %i\n", rc);
    bad |= rc >= 0;
    bad |= printc_result((void *)0x800000, 2) >= 0;
    bad |= load_result(freed) != 0;
    memset(end, 'A', 4096);
    rc = print_result(end + 4096 - 8);
    printf("syscheck: unterminated print %i\n", rc);
    bad |= rc >= 0;
    bad |= print_result("syscheck: valid print\n") != 0;
    free(end);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc >= 2 && (!strncmp(argv[1],"child",6) || argv[1][0]=='x')))
    {
        int good=argc==1 || (argc==2 && (!strncmp(argv[1],"child",6) || strlen(argv[1])==RORTHOX_COMMAND_ARGUMENT_SIZE-1)) ||
            (argc==3 && !strncmp(argv[2],"child",6)) ||
            (argc==RORTHOX_MAX_COMMAND_ARGUMENTS && !strncmp(argv[argc-1],"last",5));
        good &= !strncmp(argv[0],"syscheck.elf",13);
        print(good ? "syscheck: child PASS\n" : "syscheck: child FAIL\n");
        return !good;
    }
    if (argc != 2) return 1;
    if (!strncmp(argv[1],"cmd",3))
    {
        armed(); command_test(argv[1]+3);
        print("syscheck: continued\n"); return 0;
    }
    if (!strncmp(argv[1], "free", 4))
    {
        armed();
        print(freed_arguments_test(argv[1], argc, argv) ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: continued\n");
        return 0;
    }
    if (!strncmp(argv[1], "get", 4))
    {
        armed();
        print(arguments_test(argc, argv) ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: continued\n");
        return 0;
    }
    if (!strncmp(argv[1], "ptr", 4))
    {
        armed();
        print(pointer_test() ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: continued\n");
        return 0;
    }
    if (!strncmp(argv[1], "badsp", 6))
    {
        armed();
        int rc = bad_stack(SYSCALL_GET_PROGRAM_ARGUMENTS, 0xfffffffeU);
        if (get_arguments((void *)0xfffffffcU) != -2) rc = 1;
        printf("syscheck: result %i\n", rc);
        print("syscheck: continued\n");
        return 0;
    }
    struct command_argument *node = malloc(sizeof(*node));
    if (!node) return 1;
    memset(node, 0, sizeof(*node));
    strcpy(node->argument, argv[1]);
    armed();
    int result = rorthox_system(node);
    printf("syscheck: result %i\n", result);
    print("syscheck: continued\n");
    free(node);
    return 0;
}
