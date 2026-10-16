/* Reuse the real-handler load/rollback fixture; add malformed lists and snapshot lifetime. */
#define main allocation_rollback_main
#include "system-command.c"
#undef main
unsigned int alarm(unsigned int);
static void *snapshot;
static int loaded, forbidden_read, user_links, copied_nodes, switch_with_snapshot;
static uint32_t forbidden;
void *__real_kzalloc(size_t);
void *__wrap_kzalloc(size_t size)
{
    void *p=__real_kzalloc(size);
    if(size==RORTHOX_MAX_COMMAND_ARGUMENTS*sizeof(struct command_argument)) snapshot=p;
    return p;
}
int __real_task_copy_from_user(struct task *,void *,uint32_t,uint32_t) __attribute__((weak));
int __wrap_task_copy_from_user(struct task *task,void *buffer,uint32_t address,uint32_t size)
{
    if(address==forbidden && forbidden) forbidden_read++;
    if(size==sizeof(struct command_argument)) copied_nodes++;
    return __real_task_copy_from_user ? __real_task_copy_from_user(task,buffer,address,size) : -EINVARG;
}
int __real_process_switch(struct process *);
int __wrap_process_switch(struct process *p)
{
    if(snapshot && kalloc_is_live(snapshot)) switch_with_snapshot++;
    return __real_process_switch(p);
}
int __real_process_load_with_arguments(const char *,struct process **,struct command_argument *);
int __wrap_process_load_with_arguments(const char *f,struct process **p,struct command_argument *a)
{
    loaded++;
    struct command_argument *n=a;
    for(int i=0;n && i<=RORTHOX_MAX_COMMAND_ARGUMENTS;i++,n=n->next)
    {
        if(!snapshot || (unsigned long)n < (unsigned long)snapshot ||
           (unsigned long)n-(unsigned long)snapshot >= RORTHOX_MAX_COMMAND_ARGUMENTS*sizeof(*n))
        { user_links++; break; }
    }
    return __real_process_load_with_arguments(f,p,a);
}
static int command_case(int mode)
{
    alarm(3);
    struct host_snapshot before,after;
    struct process *caller=setup(1,&before);
    struct command_argument *list=process_malloc(caller,(RORTHOX_MAX_COMMAND_ARGUMENTS+1)*sizeof(*list));
    for(int i=0;i<=RORTHOX_MAX_COMMAND_ARGUMENTS;i++)
    {
        strcpy(list[i].argument,i==0?"blank.elf":i==RORTHOX_MAX_COMMAND_ARGUMENTS-1?"last":"child");
        list[i].next=i<RORTHOX_MAX_COMMAND_ARGUMENTS-1?&list[i+1]:0;
    }
    uint32_t *operand=(void *)(unsigned long)caller->task->registers.esp;
    operand[0]=(uint32_t)(unsigned long)list;
    int good=mode==0 || mode==1 || mode==12 || mode==13 || mode==14;
    int count=RORTHOX_MAX_COMMAND_ARGUMENTS;
    if(mode==0) {list[0].next=0;count=1;}
    if(mode==2) operand[0]=0;
    if(mode==3) list[0].argument[0]=0;
    if(mode==4) memset(list[0].argument,'x',sizeof(list[0].argument));
    if(mode==5) memset(list[1].argument,'x',sizeof(list[1].argument));
    if(mode==6) list[1].next=(void *)0xfffff000U;
    if(mode==7) operand[0]=0xffffff00U;
    if(mode==8) list[RORTHOX_MAX_COMMAND_ARGUMENTS-1].next=&list[RORTHOX_MAX_COMMAND_ARGUMENTS];
    if(mode==9) list[0].next=list;
    if(mode==10) {list[0].next=&list[1];list[1].next=list;}
    if(mode==11)
    {
        forbidden=0xffffe000U;
        list[RORTHOX_MAX_COMMAND_ARGUMENTS-1].next=(void *)(unsigned long)forbidden;
    }
    if(mode==12) /* Owned readable nodes need no W permission. */
        paging_map_to(caller->task->page_directory,list,list,(char *)list+(RORTHOX_MAX_COMMAND_ARGUMENTS+1)*sizeof(*list),5);
    if(mode==13) {static struct process foreign;process_switch(&foreign);}
    if(mode==14) {memset(list[1].argument,'x',sizeof(list[1].argument));list[1].argument[sizeof(list[1].argument)-1]=0;}
    if(mode==15) caller->task->registers.esp=0xfffffffeU;
    if(mode==16) list[RORTHOX_MAX_COMMAND_ARGUMENTS-1].next=&list[RORTHOX_MAX_COMMAND_ARGUMENTS];
    if(mode==17) list[0].next=list;
    if(mode==18) memset(list[1].argument,'x',sizeof(list[1].argument));
    if(mode==19)
    {
        list[0].next=0;
        for(int i=0;i<RORTHOX_MAX_PROGRAM_ALLOCATIONS;i++)
            if(caller->allocations[i].ptr==list) caller->allocations[i].size=sizeof(*list)-1;
    }
    struct command_argument saved[RORTHOX_MAX_COMMAND_ARGUMENTS+1];memcpy(saved,list,sizeof(saved));
    host_reset();loaded=0;snapshot=0;user_links=0;copied_nodes=0;switch_with_snapshot=0;
    host_snapshot_take(&before,message,sizeof(message));
    void *volatile rc=0;volatile int started=0;host_task_return_armed=1;
    if(!setjmp(host_task_return_jump))
        rc=mode>=16 && mode<=18?(void *)(long)process_inject_arguments(caller,list):isr80h_command7_invoke_system_command(0);
    else started=1;
    host_task_return_armed=0;
    int bad=0;
    if(good)
    {
        struct process *child=process_get(1);
        bad=!started || !child || !snapshot || kalloc_is_live(snapshot) || user_links || copied_nodes!=count || switch_with_snapshot;
        if(child)
        {
            bad |= child->arguments.argc!=count;
            for(int i=0;i<child->arguments.argc && i<count;i++)
            {
                char *a=child->arguments.argv[i];
                bad |= !kalloc_is_live(a) || strncmp(a,saved[i].argument,sizeof(saved[i].argument));
            }
        }
    }
    else
    {
        host_snapshot_take(&after,message,sizeof(message));
        bad=started || (int)(long)rc!=-EINVARG || loaded || forbidden_read ||
            host_snapshot_compare(&before,&after,message,sizeof(message));
    }
    bad |= memcmp(list,saved,sizeof(saved))!=0 || host_open_files || kalloc_bad_frees() || kalloc_poison_errors();
    printf("command case %d started=%d result=%d loads=%d forbidden=%d failed=%d\n",mode,started,(int)(long)rc,loaded,forbidden_read,bad);
    return bad;
}
int main(int argc,char **argv)
{
    int fd=argc==2?open(argv[1],O_RDONLY):-1;
    blank_size=fd<0?-1:read(fd,blank,sizeof(blank));if(blank_size<52)return 2;close(fd);
    int failures=0;
    for(int i=0;i<20;i++)
    {
        fflush(0);int pid=fork();if(!pid){int rc=command_case(i);fflush(0);_exit(rc);}
        int status;waitpid(pid,&status,0);
        if(!WIFEXITED(status)||WEXITSTATUS(status)){printf("FAIL command case %d status=%d\n",i,status);failures++;}
    }
    printf("command-arguments host: %d of 20 failed\n",failures);
    /* Includes every snapshot/child allocation, open, stat, read and map failure. */
    return allocation_rollback_main(argc,argv) || failures;
}
