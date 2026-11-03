/*
 * File syscall test for scripts/tests/file-syscalls.sh (only copied into its test image, never
 * into bin/os.bin). Every mode prints one line per print call and ends with "filetest: end
 * <mode>", after "filetest: <mode> PASS" or "filetest: <mode> FAIL <what>".
 *
 *   filetest.elf write     DATA.BIN: 70000 bytes written in 6000-byte requests (each call
 *                          moves 1-4096 bytes), stat, append "tail\n" after a seek to 0, signed
 *                          seeks, read back in 5000-byte requests, EOF, a short read leaves the
 *                          rest of its buffer alone. OLDFILE.TXT truncated and
 *                          rewritten as "new\n". BLANK.ELF copied to COPY.ELF. SUB/NOTE.TXT
 *                          created in an existing directory. The first open must give handle 1:
 *                          an old kernel without the file syscalls fails here at once.
 *   filetest.elf errors    bad handles, bad and unterminated paths (also one whose first 107
 *                          bytes name an existing file), pointers to kernel memory, freed memory,
 *                          read-only memory and past the end of an allocation or the stack
 *                          (nothing is read, written or moved), a buffer across two
 *                          allocations, a bad second or third stack operand, wrong modes,
 *                          the 17th open (-EMFILE, its WRITE target not truncated), a read-only
 *                          file, a directory, missing files and parents.
 *   filetest.elf nospace   BIG.BIN written in 4096-byte calls until -ENOSPC; the position and
 *                          size stay; a write into an existing file's last cluster still works.
 *                          Prints "filetest: nospace bytes <n>".
 *   filetest.elf fullroot  empty root files Fnn.TXT until -ENOSPC; prints "filetest: fullroot
 *                          created <n>"; writing an existing file still works.
 *   filetest.elf parent    opens KEEP.TXT, starts "filetest.elf child <handle>" and waits (sleeping,
 *                          handle open) until the child has written CHILD.TXT; the handle still
 *                          reads afterwards.
 *   filetest.elf child <h> the parent's handle number is not valid here; its own first open is
 *                          handle 1; writes CHILD.TXT.
 *   filetest.elf hold      opens 16 handles, prints "filetest: hold armed", waits for a key and
 *                          exits without closing them.
 *   filetest.elf crash     the same ("filetest: crash armed"), then raises #UD instead of exiting.
 *   filetest.elf verify    reads back what "write" left (after a reboot).
 *
 * No large globals: big buffers come from malloc.
 */
#include <stdint.h>
#include "rorthox.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "memory.h"

int bad_stack(int command, unsigned int sp);

#define DATA_SIZE 70000
#define BUF_SIZE 8192
#define DEEP_PREFIX_DEPTH 49

static char *buf;
static char *mode;

static int fail(const char *what, int value)
{
    printf("filetest: %s FAIL ", mode);
    print(what);
    printf(" (%i)\n", value);
    return 1;
}

#define CHECK(cond, what, value) do { if (!(cond)) return fail(what, value); } while (0)

static unsigned char data_byte(int i)
{
    return (unsigned char)(i * 7 + i / 251);
}

static int is(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b) + 1) == 0;
}

static int number(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9')
    {
        n = n * 10 + (*s++ - '0');
    }
    return n;
}

// Read all of a file (at most max bytes) into out; returns the byte count or a negative error.
static int read_all(const char *path, char *out, int max)
{
    int fd = rorthox_open(path, RORTHOX_FILE_READ);
    if (fd <= 0)
    {
        return fd ? fd : -100;
    }
    int total = 0;
    while (total < max)
    {
        int n = rorthox_read(fd, out + total, max - total);
        if (n < 0)
        {
            rorthox_close(fd);
            return n;
        }
        if (n == 0)
        {
            break;
        }
        total += n;
    }
    rorthox_close(fd);
    return total;
}

static int same(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

static int write_mode(void)
{
    int fd = rorthox_open("0:/DATA.BIN", RORTHOX_FILE_WRITE);
    CHECK(fd == 1, "first open did not give handle 1", fd);
    int total = 0;
    while (total < DATA_SIZE)
    {
        int ask = DATA_SIZE - total < 6000 ? DATA_SIZE - total : 6000;
        for (int i = 0; i < ask; i++)
        {
            buf[i] = data_byte(total + i);
        }
        int n = rorthox_write(fd, buf, ask);
        CHECK(n > 0 && n <= 4096 && n <= ask, "write moved a wrong count", n);
        total += n;
    }
    struct rorthox_file_stat st;
    CHECK(rorthox_stat(fd, &st) == 0 && st.filesize == DATA_SIZE && st.flags == 0, "stat after write", st.filesize);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    CHECK(rorthox_close(fd) == -EBADF, "second close", 0);

    fd = rorthox_open("0:/DATA.BIN", RORTHOX_FILE_APPEND);
    CHECK(fd == 1, "append open", fd);
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_CUR) == DATA_SIZE, "append starts at the end", rorthox_seek(fd, 0, RORTHOX_SEEK_CUR));
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_SET) == 0, "seek to 0", 0);
    CHECK(rorthox_write(fd, "tail\n", 5) == 5, "append write", 0);
    CHECK(rorthox_stat(fd, &st) == 0 && st.filesize == DATA_SIZE + 5, "stat after append", st.filesize);
    CHECK(rorthox_seek(fd, -5, RORTHOX_SEEK_END) == DATA_SIZE, "seek -5 from the end", 0);
    CHECK(rorthox_seek(fd, 1, RORTHOX_SEEK_END) == -EINVARG, "seek past the end", 0);
    CHECK(rorthox_seek(fd, -DATA_SIZE - 1, RORTHOX_SEEK_CUR) == -EINVARG, "seek before 0", 0);
    CHECK(rorthox_seek(fd, 0, 3) == -EINVARG, "seek origin 3", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);

    fd = rorthox_open("0:/DATA.BIN", RORTHOX_FILE_READ);
    CHECK(fd == 1, "read open", fd);
    total = 0;
    while (1)
    {
        int n = rorthox_read(fd, buf, 5000);
        CHECK(n >= 0 && n <= 4096, "read moved a wrong count", n);
        if (!n)
        {
            break;
        }
        for (int i = 0; i < n; i++)
        {
            int at = total + i;
            unsigned char want = at < DATA_SIZE ? data_byte(at) : (unsigned char)"tail\n"[at - DATA_SIZE];
            CHECK((unsigned char)buf[i] == want, "read back a wrong byte at", at);
        }
        total += n;
    }
    CHECK(total == DATA_SIZE + 5, "read back a wrong size", total);
    CHECK(rorthox_read(fd, buf, 1) == 0, "read at EOF", 0);
    // A count larger than what is left: only the 5 bytes read reach the buffer, the rest of the
    // requested range keeps its canary.
    for (int i = 0; i < BUF_SIZE; i++)
    {
        buf[i] = (char)(0x5A ^ (i * 37) ^ (i >> 8));
    }
    CHECK(rorthox_seek(fd, DATA_SIZE, RORTHOX_SEEK_SET) == DATA_SIZE, "seek to the tail", 0);
    CHECK(rorthox_read(fd, buf, 4000) == 5 && same(buf, "tail\n", 5), "read of the last 5 bytes with count 4000", 0);
    for (int i = 5; i < BUF_SIZE; i++)
    {
        CHECK(buf[i] == (char)(0x5A ^ (i * 37) ^ (i >> 8)), "a short read wrote past its count at", i);
    }
    CHECK(rorthox_close(fd) == 0, "close", 0);

    fd = rorthox_open("0:/OLDFILE.TXT", RORTHOX_FILE_WRITE);
    CHECK(fd == 1, "truncating open", fd);
    CHECK(rorthox_stat(fd, &st) == 0 && st.filesize == 0, "size after truncate", st.filesize);
    CHECK(rorthox_write(fd, "new\n", 4) == 4, "rewrite", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);

    int in = rorthox_open("0:/BLANK.ELF", RORTHOX_FILE_READ);
    int out = rorthox_open("0:/COPY.ELF", RORTHOX_FILE_WRITE);
    CHECK(in == 1 && out == 2, "copy opens", out);
    total = 0;
    while (1)
    {
        int n = rorthox_read(in, buf, BUF_SIZE);
        CHECK(n >= 0, "copy read", n);
        if (!n)
        {
            break;
        }
        for (int done = 0; done < n;)
        {
            int w = rorthox_write(out, buf + done, n - done);
            CHECK(w > 0, "copy write", w);
            done += w;
        }
        total += n;
    }
    CHECK(rorthox_stat(out, &st) == 0 && st.filesize == total && total > 0, "copy size", total);
    CHECK(rorthox_close(in) == 0 && rorthox_close(out) == 0, "copy close", 0);

    fd = rorthox_open("0:/SUB/NOTE.TXT", RORTHOX_FILE_APPEND);
    CHECK(fd == 1, "create in SUB", fd);
    CHECK(rorthox_write(fd, "note\n", 5) == 5, "note write", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    printf("filetest: write copied %i bytes\n", total);
    return 0;
}

// The deep path: "0:/" + "A/" x 49 + "TT.TXT" is 107 bytes, so with its NUL it fits the
// kernel's 108-byte path buffer exactly. With more bytes and no NUL in the first 108 it must be
// refused, not cut down to that existing file.
static void deep_path(char *out, int extra)
{
    int n = 0;
    memcpy(out, "0:/", 3);
    n = 3;
    for (int i = 0; i < DEEP_PREFIX_DEPTH; i++)
    {
        out[n++] = 'A';
        out[n++] = '/';
    }
    memcpy(out + n, "TT.TXT", 6);
    n += 6;
    for (int i = 0; i < extra; i++)
    {
        out[n++] = 'X';
    }
    out[n] = 0;
}

// The word after a 4096-byte allocation, made unowned by freeing the allocation behind it. The
// freed page keeps the given value in every word, which is what an unchecked read would see.
static unsigned int edge(char **keep, unsigned int fill)
{
    char *a = malloc(4096);
    char *b = malloc(4096);
    if (!a || !b)
    {
        return 0;
    }
    for (int i = 0; i < 4096 / 4; i++)
    {
        ((unsigned int *)b)[i] = fill;
    }
    free(b);
    *keep = a;
    return (unsigned int)a + 4096;
}

static int errors_mode(void)
{
    struct rorthox_file_stat st;
    // Operands: the first one(s) valid, the next one past the caller's memory.
    char *a;
    unsigned int end = edge(&a, 0);
    CHECK(end, "malloc", 0);
    ((unsigned int *)(end - 4))[0] = (unsigned int)"0:/KEEP.TXT";
    CHECK(bad_stack(SYSCALL_FILE_OPEN, end - 4) == -EINVARG, "open with an unowned mode operand", 0);
    free(a);
    int fd = rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ);
    CHECK(fd == 1, "open KEEP.TXT", fd);
    unsigned int *w;
    end = edge(&a, 0);
    w = (unsigned int *)(end - 8);
    w[0] = fd;
    w[1] = (unsigned int)buf;
    CHECK(bad_stack(SYSCALL_FILE_READ, end - 8) == -EINVARG, "read with an unowned count operand", 0);
    w[1] = 0;
    CHECK(bad_stack(SYSCALL_FILE_SEEK, end - 8) == -EINVARG, "seek with an unowned origin operand", 0);
    free(a);
    end = edge(&a, (unsigned int)&st);
    w = (unsigned int *)(end - 4);
    w[0] = fd;
    CHECK(bad_stack(SYSCALL_FILE_STAT, end - 4) == -EINVARG, "stat with an unowned buffer operand", 0);
    free(a);
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_CUR) == 0, "position moved", 0);
    int wfd = rorthox_open("0:/SCRATCH.TXT", RORTHOX_FILE_WRITE);
    CHECK(wfd == 2, "open SCRATCH.TXT", wfd);
    end = edge(&a, 0);
    w = (unsigned int *)(end - 8);
    w[0] = wfd;
    w[1] = (unsigned int)"stack";
    CHECK(bad_stack(SYSCALL_FILE_WRITE, end - 8) == -EINVARG, "write with an unowned count operand", 0);
    free(a);

    // Handles.
    CHECK(rorthox_close(0) == -EBADF && rorthox_close(17) == -EBADF && rorthox_close(-1) == -EBADF, "close of a bad handle", 0);
    CHECK(rorthox_read(5, buf, 1) == -EBADF && rorthox_write(16, buf, 1) == -EBADF, "read/write of a closed handle", 0);
    CHECK(rorthox_seek(3, 0, RORTHOX_SEEK_SET) == -EBADF && rorthox_stat(4, &st) == -EBADF, "seek/stat of a closed handle", 0);
    CHECK(rorthox_read(wfd, buf, 0) == -EBADF && rorthox_write(fd, buf, 0) == -EBADF, "wrong mode", 0);
    CHECK(rorthox_read(fd, 0, 0) == 0 && rorthox_write(wfd, 0, 0) == 0, "zero counts", 0);

    // Paths.
    char *path = malloc(256);
    CHECK(path != 0, "malloc", 0);
    CHECK(rorthox_open(0, RORTHOX_FILE_READ) == -EINVARG, "NULL path", 0);
    CHECK(rorthox_open((const char *)0x100000, RORTHOX_FILE_READ) == -EINVARG, "kernel path", 0);
    CHECK(rorthox_open("0:/KEEP.TXT", 3) == -EINVARG, "mode 3", 0);
    deep_path(path, 40);
    CHECK(rorthox_open(path, RORTHOX_FILE_WRITE) == -EINVARG, "a path without NUL in 108 bytes", 0);
    deep_path(path, 0);
    int deep = rorthox_open(path, RORTHOX_FILE_READ);
    CHECK(deep > 0, "the 107-byte path", deep);
    CHECK(rorthox_read(deep, buf, 100) == 5 && same(buf, "keep\n", 5), "the deep file changed", 0);
    CHECK(rorthox_close(deep) == 0, "close", 0);
    char *p = malloc(4096);
    char *q = malloc(4096);
    CHECK(p && q, "malloc", 0);
    free(q);
    memcpy(p + 4096 - 12, "0:/KEEP.TXT", 12);
    int k = rorthox_open(p + 4096 - 12, RORTHOX_FILE_READ);
    CHECK(k > 0, "a path ending at the end of an allocation", k);
    rorthox_close(k);
    memset(p + 4096 - 11, 'K', 11);
    CHECK(rorthox_open(p + 4096 - 11, RORTHOX_FILE_READ) == -EINVARG, "a path unterminated at the end of an allocation", 0);
    CHECK(rorthox_open("0:/NOPE.TXT", RORTHOX_FILE_READ) == -ENOENT, "missing file", 0);
    CHECK(rorthox_open("0:/NODIR/X.TXT", RORTHOX_FILE_WRITE) == -ENOENT, "missing parent", 0);
    CHECK(rorthox_open("0:/TOOLONGNAME.TXT", RORTHOX_FILE_WRITE) == -EINVARG, "long name", 0);
    CHECK(rorthox_open("0:/A B", RORTHOX_FILE_WRITE) == -EINVARG, "name with a space", 0);
    CHECK(rorthox_open("KEEP.TXT", RORTHOX_FILE_READ) == -EINVARG, "relative path", 0);
    CHECK(rorthox_open("0:/SUB", RORTHOX_FILE_READ) == -EINVARG && rorthox_open("0:/SUB", RORTHOX_FILE_WRITE) == -EINVARG, "directory", 0);
    CHECK(rorthox_open("0:/RO.TXT", RORTHOX_FILE_WRITE) == -ERDONLY && rorthox_open("0:/RO.TXT", RORTHOX_FILE_APPEND) == -ERDONLY, "read-only file", 0);

    // Buffers: nothing read, written or moved when any byte is bad.
    char *freed = malloc(4096);
    free(freed);
    memset(p, 'Z', 4096);
    CHECK(rorthox_read(fd, p, 4097) == -EINVARG, "read past the end of an allocation", 0);
    CHECK(p[0] == 'Z' && p[4095] == 'Z', "a failed read wrote", 0);
    CHECK(rorthox_read(fd, (void *)"constant", 4) == -EINVARG, "read into read-only memory", 0);
    CHECK(rorthox_read(fd, (void *)0x100000, 4) == -EINVARG, "read into the kernel", 0);
    CHECK(rorthox_read(fd, freed, 4) == -EINVARG, "read into freed memory", 0);
    CHECK(rorthox_read(fd, 0, 1) == -EINVARG, "read into NULL", 0);
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_CUR) == 0, "a failed read moved", 0);
    CHECK(rorthox_stat(fd, (void *)"constant") == -EINVARG && rorthox_stat(fd, (void *)freed) == -EINVARG &&
          rorthox_stat(fd, (void *)0x100000) == -EINVARG, "stat into bad memory", 0);
    CHECK(rorthox_stat(fd, &st) == 0 && st.filesize == 5, "stat", st.filesize);
    // A buffer may span two allocations (and the page edge between them).
    char *r = malloc(4096);
    CHECK(r == p + 4096, "two adjacent allocations", 0);
    CHECK(rorthox_read(fd, r - 2, 5) == 5 && same(r - 2, "keep\n", 5), "read across two allocations", 0);
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_SET) == 0, "seek", 0);
    free(r);
    CHECK(rorthox_write(wfd, p, 4097) == -EINVARG, "write past the end of an allocation", 0);
    CHECK(rorthox_write(wfd, freed, 10) == -EINVARG && rorthox_write(wfd, (void *)0x100000, 1) == -EINVARG &&
          rorthox_write(wfd, 0, 1) == -EINVARG, "write from bad memory", 0);
    CHECK(rorthox_write(wfd, p, 0x80000000U) == -EINVARG, "count above INT32_MAX", 0);
    // The stack's last page is owned, the readable page after it isn't: the first 4096 bytes alone
    // would copy.
    CHECK(rorthox_write(wfd, (void *)(RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START - 4096), 4097) == -EINVARG,
          "write past the end of the stack", 0);
    CHECK(rorthox_stat(wfd, &st) == 0 && st.filesize == 0, "a failed write wrote", st.filesize);
    CHECK(rorthox_write(wfd, "const", 5) == 5, "write from read-only memory", 0);
    CHECK(rorthox_close(fd) == 0 && rorthox_close(wfd) == 0, "close", 0);

    // 16 handles, then -EMFILE without touching the WRITE target.
    for (int i = 1; i <= 16; i++)
    {
        int h = rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ);
        CHECK(h == i, "handle numbers 1-16", h);
    }
    CHECK(rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_WRITE) == -EMFILE, "17th open (WRITE)", 0);
    CHECK(rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ) == -EMFILE, "17th open (READ)", 0);
    CHECK(rorthox_close(7) == 0 && rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ) == 7, "lowest free handle reused", 0);
    CHECK(rorthox_read(16, buf, 100) == 5 && same(buf, "keep\n", 5), "KEEP.TXT changed", 0);
    for (int i = 1; i <= 16; i++)
    {
        CHECK(rorthox_close(i) == 0, "close of handle", i);
    }
    free(p);
    free(path);
    return 0;
}

static int nospace_mode(void)
{
    int fd = rorthox_open("0:/BIG.BIN", RORTHOX_FILE_WRITE);
    CHECK(fd == 1, "open BIG.BIN", fd);
    memset(buf, 'B', 4096);
    int total = 0, n;
    while ((n = rorthox_write(fd, buf, 4096)) == 4096)
    {
        total += n;
    }
    CHECK(n == -ENOSPC, "the write that did not fit", n);
    CHECK(total > 0 && total % 65536 == 0, "bytes written before -ENOSPC", total);
    struct rorthox_file_stat st;
    CHECK(rorthox_stat(fd, &st) == 0 && st.filesize == total, "size after -ENOSPC", st.filesize);
    CHECK(rorthox_seek(fd, 0, RORTHOX_SEEK_CUR) == total, "position after -ENOSPC", 0);
    CHECK(rorthox_write(fd, buf, 1) == -ENOSPC, "one more byte", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    fd = rorthox_open("0:/SCRATCH.TXT", RORTHOX_FILE_APPEND);
    CHECK(fd == 1 && rorthox_write(fd, "+more", 5) == 5, "write into a cluster the file has", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    printf("filetest: nospace bytes %i\n", total);
    return 0;
}

static int fullroot_mode(void)
{
    char name[16];
    memcpy(name, "0:/F00.TXT", 11);
    int created = 0, fd = 0;
    for (int i = 0; i < 100; i++)
    {
        name[4] = '0' + i / 10;
        name[5] = '0' + i % 10;
        fd = rorthox_open(name, RORTHOX_FILE_WRITE);
        if (fd < 0)
        {
            break;
        }
        rorthox_close(fd);
        created++;
    }
    CHECK(fd == -ENOSPC && created > 0, "the create that did not fit", fd);
    CHECK(rorthox_open(name, RORTHOX_FILE_APPEND) == -ENOSPC, "APPEND create in a full root", 0);
    fd = rorthox_open("0:/SCRATCH.TXT", RORTHOX_FILE_APPEND);
    CHECK(fd == 1 && rorthox_write(fd, "+root", 5) == 5, "write to a file in a full root", 0);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    printf("filetest: fullroot created %i\n", created);
    return 0;
}

static int parent_mode(void)
{
    int h = rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ);
    CHECK(h > 0, "open KEEP.TXT", h);
    char command[40];
    memcpy(command, "filetest.elf child ", 19);
    char *digits = itoa(h);
    strcpy(command + 19, digits);
    rorthox_system_run(command);
    // Sleep, holding the handle, until the child has written CHILD.TXT.
    int seen = 0;
    for (int i = 0; i < 6000 && !seen; i++)
    {
        int c = rorthox_open("0:/CHILD.TXT", RORTHOX_FILE_READ);
        if (c > 0)
        {
            seen = 1;
            rorthox_close(c);
        }
        else
        {
            sleep(10);
        }
    }
    CHECK(seen, "CHILD.TXT never appeared", 0);
    CHECK(rorthox_read(h, buf, 100) == 5 && same(buf, "keep\n", 5), "the parent's handle after the child", 0);
    CHECK(rorthox_close(h) == 0, "close", 0);
    return 0;
}

static int child_mode(int h)
{
    struct rorthox_file_stat st;
    CHECK(rorthox_read(h, buf, 5) == -EBADF, "the parent's handle reads in the child", h);
    CHECK(rorthox_stat(h, &st) == -EBADF && rorthox_close(h) == -EBADF, "the parent's handle in the child", h);
    int own = rorthox_open("0:/KEEP.TXT", RORTHOX_FILE_READ);
    CHECK(own == 1, "the child's first handle", own);
    CHECK(rorthox_read(own, buf, 100) == 5, "child read", 0);
    rorthox_close(own);
    int fd = rorthox_open("0:/CHILD.TXT", RORTHOX_FILE_WRITE);
    CHECK(fd == 1 && rorthox_write(fd, "child\n", 6) == 6, "CHILD.TXT", fd);
    CHECK(rorthox_close(fd) == 0, "close", 0);
    return 0;
}

static int hold_mode(int crash)
{
    for (int i = 1; i <= 16; i++)
    {
        int h = rorthox_open("0:/DATA.BIN", RORTHOX_FILE_READ);
        CHECK(h == i, "hold open", h);
    }
    print(crash ? "filetest: crash armed\n" : "filetest: hold armed\n");
    rorthox_getkeyblock();
    if (crash)
    {
        print("filetest: raising #UD\n");
        __asm__ volatile("ud2");
    }
    return 0;
}

static int verify_mode(void)
{
    char *data = malloc(DATA_SIZE + 100);
    CHECK(data != 0, "malloc", 0);
    int n = read_all("0:/DATA.BIN", data, DATA_SIZE + 100);
    CHECK(n == DATA_SIZE + 5, "DATA.BIN size", n);
    for (int i = 0; i < DATA_SIZE; i++)
    {
        CHECK((unsigned char)data[i] == data_byte(i), "DATA.BIN byte", i);
    }
    CHECK(same(data + DATA_SIZE, "tail\n", 5), "DATA.BIN tail", 0);
    CHECK(read_all("0:/OLDFILE.TXT", data, 100) == 4 && same(data, "new\n", 4), "OLDFILE.TXT", 0);
    CHECK(read_all("0:/SUB/NOTE.TXT", data, 100) == 5 && same(data, "note\n", 5), "SUB/NOTE.TXT", 0);
    CHECK(read_all("0:/CHILD.TXT", data, 100) == 6 && same(data, "child\n", 6), "CHILD.TXT", 0);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    buf = malloc(BUF_SIZE);
    if (argc < 2 || !buf)
    {
        print("filetest: usage: filetest.elf <mode>\n");
        return 1;
    }
    mode = argv[1];
    int res = 1;
    if (is(mode, "write")) res = write_mode();
    else if (is(mode, "errors")) res = errors_mode();
    else if (is(mode, "nospace")) res = nospace_mode();
    else if (is(mode, "fullroot")) res = fullroot_mode();
    else if (is(mode, "parent")) res = parent_mode();
    else if (is(mode, "child") && argc == 3) res = child_mode(number(argv[2]));
    else if (is(mode, "hold")) res = hold_mode(0);
    else if (is(mode, "crash")) res = hold_mode(1);
    else if (is(mode, "verify")) res = verify_mode();
    else fail("unknown mode", 0);
    if (!res)
    {
        printf("filetest: %s PASS\n", mode);
    }
    printf("filetest: end %s\n", mode);
    return res;
}
