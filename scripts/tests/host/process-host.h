/*
 * Stand-ins for what kernel/task/process.c, kernel/task/task.c and kernel/isr80h/process.c call
 * outside themselves, for the host checks of loading a process (scripts/tests/load-unwind.sh,
 * scripts/tests/argument-oom.sh). The heap is scripts/tests/host/kalloc.c.
 *
 * The file layer serves one file from memory and can fail its n-th fopen, fstat or fread. The
 * paging layer gives every page directory one allocation, can fail its n-th mapping, remembers
 * which live allocations each directory maps, and notes when a directory is freed after memory
 * it maps, when a freed directory is used, and every directory loaded with paging_switch().
 * task_return() jumps back to the check through host_task_return_jump.
 */
#ifndef HOST_PROCESS_HOST_H
#define HOST_PROCESS_HOST_H

#include <setjmp.h>
#include <stdint.h>

// kernel/string/string.c brings its own snprintf (which calls itself through vsprintf), so the
// checks format with the C library's checked variant instead.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define host_snprintf(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)

struct paging_4gb_chunk;
struct registers;

// The file every fopen() opens.
void host_file_serve(const void *data, int size);
// Make the n-th call (counted from the last host_reset) fail. 0 = never.
extern int host_fail_open_at;
extern int host_fail_stat_at;
extern int host_fail_read_at;
extern int host_fail_map_at;
// Calls so far, and 1 once the chosen call failed.
extern int host_open_calls;
extern int host_stat_calls;
extern int host_read_calls;
extern int host_map_calls;
extern int host_failed;
// Files open right now.
extern int host_open_files;

// Every mapping paging_map_to() made since the last host_reset (not the unmappings), in order.
#define HOST_MAX_MAP_LOG 64
struct host_map_record
{
    struct paging_4gb_chunk *directory;
    void *virt;
    void *phys;
    void *phys_end;
    int flags;
};
extern struct host_map_record host_map_log[HOST_MAX_MAP_LOG];
extern int host_map_log_count;

// Problems found by the paging stand-ins (0 = none); host_paging_problem says which.
extern int host_paging_problems;
extern char host_paging_problem[160];

// Every directory given to paging_switch(), in order (at most 64 are kept).
extern int host_switch_count;
extern struct paging_4gb_chunk *host_switches[64];

// task_return(): if host_task_return_armed, longjmp(host_task_return_jump, 1) and keep regs.
extern int host_task_return_armed;
extern jmp_buf host_task_return_jump;
extern struct registers *host_task_return_regs;

// Lines printed with print() that contain "terminated".
extern int host_terminated_prints;

// Clear the counters and failure choices (not the file content).
void host_reset(void);

// What a failed load must leave as it was: the task list (in order, both ways), the current
// task, the process with the keyboard (current_process), the process slots, the live heap
// allocations and the open files.
void host_file_size(uint32_t size);
extern int host_short_read_at;
extern int host_read_error;
extern uint32_t host_read_bytes;

#define HOST_MAX_TASKS 16
struct host_snapshot
{
    struct task *tasks[HOST_MAX_TASKS];
    int task_count;
    struct task *head, *tail, *current_task;
    struct process *current_process;
    struct process *slots[32];
    int live;
    int open_files;
};
// Returns 0 if the list is broken (a prev link that doesn't match, or too long).
int host_snapshot_take(struct host_snapshot *s, char *message, int size);
// 0 if b equals a, else 1 and message says what differs.
int host_snapshot_compare(const struct host_snapshot *a, const struct host_snapshot *b, char *message, int size);

// Load "0:/shell.elf" (whatever host_file_serve() serves) into slot 0 as the running caller:
// its task current and its process the one with the keyboard. Exits with 2 if that fails.
struct process *host_load_caller(void);

#endif
