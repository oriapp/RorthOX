/* Real syscall 8, region ownership and copies; noncontiguous backing and atomic refusal. */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "memory/paging/paging.h"
#include "task/process.h"
#include "isr80h/process.h"
#include "loader/formats/elfloader.h"
extern int process_user_span(struct process *, uint32_t, int, uint32_t *) __attribute__((weak));
extern int task_copy_to_user(struct task *, uint32_t, const void *, uint32_t) __attribute__((weak));
extern int task_copy_from_user(struct task *, void *, uint32_t, uint32_t) __attribute__((weak));
unsigned int alarm(unsigned int);
static unsigned char blank[65536];
static int blank_size;
static int span(struct process *p, uint32_t a, int w, int expected, uint32_t length)
{
    uint32_t n = 0;
    int rc = process_user_span(p, a, w, &n);
    if (rc != expected || (!rc && n != length))
    { printf("span %x write=%d: rc=%d length=%u want=%d/%u\n", a,w,rc,n,expected,length); return 1; }
    return 0;
}
static int one(int mode)
{
    kalloc_init(); kalloc_set_alignment(4096); host_reset(); host_file_serve(blank, blank_size);
    struct process *p = host_load_caller();
    p->arguments.argc = 17; p->arguments.argv = (void *)0x123456;
    uint32_t *operand = process_malloc(p, 4);
    p->task->registers.esp = (uint32_t)(unsigned long)operand;
    /* A readable but unowned target must remain untouched. */
    if (mode == 0)
    {
        unsigned char *outside = kzalloc(32); memset(outside, 0x5a, 32);
        operand[0] = (uint32_t)(unsigned long)(outside+3);
        int rc = (int)(long)isr80h_command8_get_program_arguments(0);
        int changed = 0; for (int i=0;i<32;i++) changed |= outside[i] != 0x5a;
        printf("unowned target: result=%d changed=%d\n",rc,changed);
        return rc != -EINVARG || changed;
    }
    if (!process_user_span || !task_copy_to_user || !task_copy_from_user)
    { printf("copy/ownership API absent (separate from behavioural case 0)\n"); return 1; }
    if (mode == 1)
    {
        int bad=0; uint32_t bottom=RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_END;
        bad |= span(p,bottom,1,0,USER_PROGRAM_STACK_SIZE);
        bad |= span(p,bottom+USER_PROGRAM_STACK_SIZE-1,1,0,1);
        bad |= span(p,bottom-1,0,-EINVARG,0);
        bad |= span(p,bottom+USER_PROGRAM_STACK_SIZE,0,-EINVARG,0);
        unsigned char *mem=process_malloc(p,19); uint32_t a=(uint32_t)(unsigned long)mem;
        bad |= span(p,a+3,1,0,16); bad |= span(p,a+19,0,-EINVARG,0);
        bad |= span(p,a-1,0,-EINVARG,0);
        process_free(p,mem); bad |= span(p,a,0,-EINVARG,0);
        bad |= span(p,(uint32_t)(unsigned long)p->stack,0,-EINVARG,0);
        bad |= span(p,(uint32_t)(unsigned long)p->elf_file->elf_memory,0,-EINVARG,0);
        struct elf_header *h=elf_header(p->elf_file);
        for(int i=0;i<h->e_phnum;i++)
        {
            struct elf32_phdr *ph=elf_program_header(h,i);
            if(ph->p_type!=PT_LOAD || !ph->p_memsz) continue;
            bad |= span(p,ph->p_vaddr,0,0,ph->p_memsz);
            bad |= span(p,ph->p_vaddr,1,ph->p_flags&PF_W?0:-EINVARG,ph->p_memsz);
            /* Segment padding, unlike adjacent exact regions, is not owned. */
            if ((ph->p_vaddr+ph->p_memsz)&4095) bad |= span(p,ph->p_vaddr+ph->p_memsz,0,-EINVARG,0);
        }
        struct process *foreign=0;
        if(process_load("0:/foreign.elf",&foreign)!=0) return 1;
        void *foreign_memory=process_malloc(foreign,16);
        if(!foreign_memory) return 1;
        bad |= span(p,(uint32_t)(unsigned long)foreign_memory,0,-EINVARG,0);
        bad |= task_copy_to_user(p->task,(uint32_t)(unsigned long)operand,0,0)!=-EINVARG;
        p->filetype=PROCESS_FILETYPE_BINARY; p->size=13;
        bad |= span(p,RORTHOX_PROGRAM_VIRTUAL_ADDRESS+12,1,0,1);
        bad |= span(p,RORTHOX_PROGRAM_VIRTUAL_ADDRESS+13,0,-EINVARG,0);
        bad |= span(p,0,0,-EINVARG,0);
        return bad;
    }
    const uint32_t base=0x20000000;
    kalloc_guard_next(4096,KALLOC_GUARD_ABOVE); unsigned char *a=kzalloc(4096);
    kalloc_guard_next(4096,KALLOC_GUARD_ABOVE); unsigned char *b=kzalloc(4096);
    memset(a,0x5a,4096); memset(b,0x5a,4096);
    p->allocations[10].ptr=(void *)(unsigned long)base; p->allocations[10].size=8192;
    paging_map(p->task->page_directory,(void *)(unsigned long)base,a,7);
    paging_map(p->task->page_directory,(void *)(unsigned long)(base+4096),b, mode==3 || mode==12?5:mode==4 || mode==13?2:7);
    uint32_t target=base+4092;
    if(mode==5) p->allocations[10].size=4096; /* second half unowned */
    if(mode==6) { p->allocations[10].size=4096; p->allocations[11].ptr=(void *)(unsigned long)(base+4096); p->allocations[11].size=4096; }
    if(mode==7) { p->allocations[10].size=4096; p->allocations[11].ptr=(void *)(unsigned long)(base+4097); p->allocations[11].size=4095; }
    if(mode==8)
    {
        target=0xfffffffcU;
        p->allocations[10].ptr=(void *)0xfffff000U; p->allocations[10].size=4096;
        paging_map(p->task->page_directory,(void *)0xfffff000U,a,7);
    }
    if(mode==14)
    {
        /* The ownership boundary is inside one mapped page, not at its end. */
        target=base+128;
        p->allocations[10].ptr=(void *)(unsigned long)target;
        p->allocations[10].size=4;
    }
    if(mode==9) target=0;
    if(mode==10) { static struct process foreign; process_switch(&foreign); }
    if(mode==11) p->task->registers.esp=0xfffffffeU;
    if(mode==12 || mode==13)
    {
        unsigned char result[8]; memset(result,0x33,sizeof(result));
        int rc=task_copy_from_user(p->task,result,target,sizeof(result));
        int bad=rc!=(mode==12?0:-EINVARG);
        for(int i=0;i<8;i++) bad |= result[i]!=(mode==12?0x5a:0x33);
        return bad;
    }
    operand[0]=target;
    struct host_snapshot before,after; char msg[200]; host_reset();
    host_snapshot_take(&before,msg,sizeof(msg));
    int calls=kalloc_calls();
    extern int host_translate_calls;
    host_translate_calls=0;
    int rc=(int)(long)isr80h_command8_get_program_arguments(0);
    host_snapshot_take(&after,msg,sizeof(msg));
    int good=mode==2 || mode==6 || mode==10;
    unsigned char expected[sizeof(struct process_arguments)];
    struct process_arguments result={17,(void *)0x123456}; memcpy(expected,&result,sizeof(result));
    int bad=rc!=(good?0:-EINVARG) || host_snapshot_compare(&before,&after,msg,sizeof(msg));
    for(int i=0;i<4096;i++)
    {
        unsigned char va=good && i>=4092?expected[i-4092]:0x5a;
        unsigned char vb=good && i<(int)sizeof(result)-4?expected[i+4]:0x5a;
        /* LP64 inserts padding after argc; the guest has no such bytes. */
        int padding = good && i < (int)__builtin_offsetof(struct process_arguments, argv)-4;
        if(a[i]!=va || (!padding && b[i]!=vb)) bad=1;
    }
    if(kalloc_calls()!=calls || host_map_calls) { printf("copy allocated or changed mappings\n"); bad=1; }
    if(mode==8 && host_translate_calls!=2) { printf("wrapped target was translated\n"); bad=1; }
    if(host_switch_count) { printf("copy switched address spaces\n"); bad=1; }
    printf("destination case %d result=%d good=%d failed=%d\n",mode,rc,good,bad);
    return bad;
}
int main(int argc,char **argv)
{
    int fd=argc==2?open(argv[1],O_RDONLY):-1;
    blank_size=fd<0?-1:read(fd,blank,sizeof(blank)); if(blank_size<52) return 2; close(fd);
    int failures=0;
    for(int i=0;i<15;i++)
    {
        fflush(0); int pid=fork(); if(!pid) {alarm(3);int rc=one(i);fflush(0);_exit(rc);}
        int status;waitpid(pid,&status,0);
        if(!WIFEXITED(status)||WEXITSTATUS(status)) {printf("FAIL program-arguments case %d status=%d\n",i,status);failures++;}
    }
    printf("program-arguments host: %d of 15 failed\n",failures); return failures!=0;
}
