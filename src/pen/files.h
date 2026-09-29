#ifndef PENDESK_FILES_H
#define PENDESK_FILES_H

#include <stddef.h>
#include <stdint.h>

struct files;

struct files *
files_new(const char *host, uint16_t port, const char *socks_host, uint16_t socks_port,
          const uint8_t token[32], int notify_fd);

void files_free(struct files *files);

int files_submit(struct files *files, const char *action);

uint64_t files_state_version(const struct files *files);

#endif
