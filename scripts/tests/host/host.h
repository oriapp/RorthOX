/*
 * The few C library calls the host checks need, declared by hand: the checks include kernel
 * headers (fs/file.h, string/string.h) whose names clash with <stdio.h>, <stdlib.h> and
 * <unistd.h> (SEEK_SET, atoi, ...). Linux x86-64 only.
 */
#ifndef HOST_HOST_H
#define HOST_HOST_H

typedef int pid_t;

int printf(const char *format, ...);
int fflush(void *stream);
void perror(const char *s);
void exit(int status) __attribute__((noreturn));
void _exit(int status) __attribute__((noreturn));
pid_t fork(void);
pid_t waitpid(pid_t pid, int *status, int options);
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long count);
long pread(int fd, void *buf, unsigned long count, long offset);
int close(int fd);

#define O_RDONLY 0

// The wait status encoding of Linux.
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) ((((s) & 0x7f) + 1) >> 1 > 0 && ((s) & 0x7f) != 0x7f && ((s) & 0x7f) != 0)
#define WTERMSIG(s) ((s) & 0x7f)

#endif
