#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "socks.h"
#include "io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int tcp_connect(const char *host, uint16_t port) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    int fd;
    int result;
    int error = 0;
    socklen_t size = sizeof(error);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) return -1;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    result = connect(fd, (struct sockaddr *) &address, sizeof(address));
    if ((result == 0 || (errno == EINPROGRESS && io_wait(fd, POLLOUT) == 0)) &&
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error) {
        int enabled = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        return fd;
    }
    close(fd);
    return -1;
}

static int socks_authenticate(int fd) {
    uint8_t reply[2];
    return io_write_all(fd, "\x05\x01\x00", 3) == 0 && io_read_all(fd, reply, sizeof(reply)) == 0 &&
           reply[0] == 5 && reply[1] == 0 ? 0 : -1;
}

static int socks_response(int fd, struct sockaddr_in *bound) {
    uint8_t reply[4];
    uint8_t address[16];
    uint8_t port[2];
    size_t length;
    if (io_read_all(fd, reply, sizeof(reply)) != 0 || reply[0] != 5 || reply[1] != 0) return -1;
    if (reply[3] == 1) length = 4;
    else if (reply[3] == 4) length = 16;
    else if (reply[3] == 3) {
        if (io_read_all(fd, reply, 1) != 0) return -1;
        length = reply[0];
    } else return -1;
    if (length > sizeof(address) || io_read_all(fd, address, length) != 0 ||
        io_read_all(fd, port, sizeof(port)) != 0)
        return -1;
    if (bound) {
        if (reply[3] != 1) return -1;
        *bound = (struct sockaddr_in) {.sin_family = AF_INET};
        memcpy(&bound->sin_addr, address, sizeof(bound->sin_addr));
        memcpy(&bound->sin_port, port, sizeof(bound->sin_port));
    }
    return 0;
}

static int socks_request(int fd, uint8_t command, const char *host, uint16_t port,
                         struct sockaddr_in *bound) {
    uint8_t request[10] = {5, command, 0, 1};
    uint16_t network_port = htons(port);
    if (inet_pton(AF_INET, host, request + 4) != 1) return -1;
    memcpy(request + 8, &network_port, sizeof(network_port));
    return io_write_all(fd, request, sizeof(request)) == 0 && socks_response(fd, bound) == 0 ? 0
                                                                                             : -1;
}

int socks_handshake(int fd, uint8_t command, const char *host, uint16_t port,
                    struct sockaddr_in *bound) {
    return socks_authenticate(fd) == 0 && socks_request(fd, command, host, port, bound) == 0 ? 0
                                                                                             : -1;
}
