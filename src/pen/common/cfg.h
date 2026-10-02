#ifndef CFG_H
#define CFG_H

#include <stddef.h>
#include <stdint.h>

struct cfg {
    char host[256];
    char socks_host[256];
    uint16_t port;
    uint16_t socks_port;
    uint8_t token[32];
};

int cfg_load(const char *path, struct cfg *cfg);

int cfg_control_path(char *path, size_t size, const char *config);

#endif