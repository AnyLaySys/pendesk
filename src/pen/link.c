#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "link.h"
#include "io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int connect_tcp(const char *host, uint16_t port) {
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

static int
socks_request(int fd, uint8_t command, const char *host, uint16_t port, struct sockaddr_in *bound) {
    uint8_t request[10] = {5, command, 0, 1};
    uint16_t network_port = htons(port);
    if (inet_pton(AF_INET, host, request + 4) != 1) return -1;
    memcpy(request + 8, &network_port, sizeof(network_port));
    return io_write_all(fd, request, sizeof(request)) == 0 && socks_response(fd, bound) == 0 ? 0
                                                                                             : -1;
}

static int connect_socks(const struct cfg *cfg) {
    int fd = connect_tcp(cfg->socks_host, cfg->socks_port);
    if (fd < 0) return -1;
    if (socks_authenticate(fd) != 0 || socks_request(fd, 1, cfg->host, cfg->port, NULL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void nonce(uint8_t output[8]) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t length = 0;
    if (fd >= 0) {
        while (length < 8) {
            ssize_t count = read(fd, output + length, 8 - length);
            if (count > 0) length += (size_t) count;
            else if (count < 0 && errno != EINTR) break;
            else if (!count) break;
        }
        close(fd);
    }
    if (length == 8) return;
    struct timespec now;
    uint64_t value = (uint64_t) getpid() << 32;
    clock_gettime(CLOCK_MONOTONIC, &now);
    value ^= (uint64_t) now.tv_sec << 32 | (uint32_t) now.tv_nsec;
    for (size_t index = 0; index != 8; ++index)
        output[index] = (uint8_t)(value >> (56 - index * 8));
}

void link_init(struct link *link) {
    *link = (struct link) {.fd = -1};
    atomic_init(&link->running, false);
    pthread_mutex_init(&link->write_mutex, NULL);
}

void link_destroy(struct link *link) {
    link_close(link);
    pthread_mutex_destroy(&link->write_mutex);
}

int link_open(struct link *link, const struct cfg *cfg) {
    link->fd = connect_socks(cfg);
    return link->fd < 0 ? -1 : 0;
}

void link_start(struct link *link) {
    atomic_store(&link->running, true);
}

void link_stop(struct link *link) {
    atomic_store(&link->running, false);
    if (link->fd >= 0) shutdown(link->fd, SHUT_RDWR);
}

bool link_running(const struct link *link) {
    return atomic_load(&link->running);
}

void link_close(struct link *link) {
    link_stop(link);
    if (link->fd >= 0) close(link->fd);
    link->fd = -1;
}

void link_video_close(struct video *video) {
    if (video->fd >= 0) close(video->fd);
    if (video->association >= 0) close(video->association);
    free(video->frame);
    free(video->fragments);
    *video = (struct video) {.association = -1, .fd = -1};
}

int link_video_open(struct video *video, const struct cfg *cfg) {
    struct sockaddr_in proxy;
    int association = connect_tcp(cfg->socks_host, cfg->socks_port);
    int fd;
    if (association < 0) return -1;
    if (socks_authenticate(association) != 0 ||
        socks_request(association, 3, "0.0.0.0", 0, &proxy) != 0) {
        close(association);
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *) &proxy, sizeof(proxy)) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        if (fd >= 0) close(fd);
        close(association);
        return -1;
    }
    video->association = association;
    video->fd = fd;
    return 0;
}

int link_handshake(struct link *link, const struct cfg *cfg, struct mode mode, uint8_t output[8]) {
    uint8_t hello[49] = {'P', 'D', 'S', 'K', PROTOCOL_VERSION};
    nonce(output);
    memcpy(hello + 5, cfg->token, sizeof(cfg->token));
    hello[37] = (uint8_t)(mode.width >> 8);
    hello[38] = (uint8_t) mode.width;
    hello[39] = (uint8_t)(mode.height >> 8);
    hello[40] = (uint8_t) mode.height;
    memcpy(hello + 41, output, 8);
    return io_write_all(link->fd, hello, sizeof(hello));
}

int link_hello(const struct video *video, const struct cfg *cfg) {
    uint8_t packet[55] = {0, 0, 0, 1};
    uint16_t port = htons(cfg->port);
    if (inet_pton(AF_INET, cfg->host, packet + 4) != 1) return -1;
    memcpy(packet + 8, &port, sizeof(port));
    memcpy(packet + 10, "PDSU", 4);
    packet[14] = PROTOCOL_VERSION;
    memcpy(packet + 15, cfg->token, sizeof(cfg->token));
    memcpy(packet + 47, video->nonce, sizeof(video->nonce));
    return send(video->fd, packet, sizeof(packet), 0) == (ssize_t)
    sizeof(packet) ? 0 : -1;
}

int link_send(struct link *link, const void *data, size_t length) {
    int result;
    pthread_mutex_lock(&link->write_mutex);
    result = link->fd < 0 ? -1 : io_write_all(link->fd, data, length);
    pthread_mutex_unlock(&link->write_mutex);
    if (result != 0) link_stop(link);
    return result;
}