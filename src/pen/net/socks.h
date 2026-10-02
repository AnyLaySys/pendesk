#ifndef PENDESK_SOCKS_H
#define PENDESK_SOCKS_H

#include <netinet/in.h>
#include <stdint.h>

int tcp_connect(const char *host, uint16_t port);

int socks_handshake(int fd, uint8_t command, const char *host, uint16_t port,
                    struct sockaddr_in *bound);

#endif
