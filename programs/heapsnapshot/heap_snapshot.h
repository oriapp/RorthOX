/*
 * Copies of the kernel's heap table for the out-of-memory test programs.
 *
 * The heap table is kernel memory: a program must not read it. The test script attaches a debugger
 * (scripts/tests/lib/heap_snapshot.py) through QEMU's gdbstub instead. heap_snapshot() fills the
 * program's own buffer with 0xee, then calls heap_snapshot_wait(), where the debugger stops
 * the VM. The debugger checks that the stop is in this program's task (ring 3, its own page
 * directory), reads the real table while nothing runs, writes it into the buffer through the task's own
 * mapping, writes the request's sequence number into the acknowledgement word and lets the VM run
 * on. Without a debugger nothing writes the acknowledgement, so heap_snapshot() fails: a test can't
 * pass on a stale or empty copy.
 *
 * heap_snapshot_before(wrapper) does the same and also tells the debugger which syscall wrapper the
 * program calls next. The debugger stops there once and requires the heap table to be exactly the
 * copy it gave the program: nothing was allocated or freed in between.
 *
 * heap_snapshot_done() tells the debugger how many snapshots the program asked for; it detaches
 * there. Call it once, on every path, before the program waits for more keys.
 */
#ifndef HEAP_SNAPSHOT_H
#define HEAP_SNAPSHOT_H

#include "config.h"

#define HEAP_SNAPSHOT_BLOCKS (RORTHOX_HEAP_SIZE_BYTES / RORTHOX_HEAP_BLOCK_SIZE)

// 0 when the debugger acknowledged the request, -1 if not (the copy is then unusable).
int heap_snapshot(void);
int heap_snapshot_before(void *wrapper);
void heap_snapshot_done(void);

// Free blocks in the copy.
int heap_snapshot_free(void);
// The free run in the copy starting at or after block *start. Returns its length, 0 if there is none.
int heap_snapshot_next_run(int *start);

#endif
