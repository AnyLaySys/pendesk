#ifndef IO_H
#define IO_H

#include <signal.h>
#include <stddef.h>

extern volatile sig_atomic_t alive;

int io_wait(int fd, short events);

int io_read_all(int fd, void *data, size_t length);

int io_write_all(int fd, const void *data, size_t length);

#endif