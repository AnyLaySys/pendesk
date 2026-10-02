#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "link.h"
#include "io.h"
#include "nonce.h"
#include "socks.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_socks(const struct cfg *cfg) {
    int fd = tcp_connect(cfg->socks_host, cfg->socks_port);
    if (fd < 0) return -1;
    if (socks_handshake(fd, 1, cfg->host, cfg->port, NULL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
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
    video_frames_free(&video->frames);
    *video = (struct video) {.association = -1, .fd = -1};
}

int link_video_open(struct video *video, const struct cfg *cfg) {
    struct sockaddr_in proxy;
    int association = tcp_connect(cfg->socks_host, cfg->socks_port);
    int fd;
    if (association < 0) return -1;
    if (socks_handshake(association, 3, "0.0.0.0", 0, &proxy) != 0) {
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