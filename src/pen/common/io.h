#ifndef IO_H
#define IO_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

extern volatile sig_atomic_t alive;

uint64_t milliseconds(void);

void io_stop_process(pid_t process);

int io_wait(int fd, short events);

int io_read_all(int fd, void *data, size_t length);

int io_write_all(int fd, const void *data, size_t length);

#endif
