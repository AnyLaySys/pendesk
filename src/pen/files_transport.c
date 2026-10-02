#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "files_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int files_wait_fd(int fd, short events) {
    struct pollfd pollfd = {.fd = fd, .events = events};
    while (alive) {
        int result = poll(&pollfd, 1, 3000);
        if (result > 0) return 0;
        if (result == 0 || (result < 0 && errno != EINTR)) return -1;
    }
    errno = EINTR;
    return -1;
}

int files_read_all(int fd, void *data, size_t length) {
    uint8_t *bytes = data;
    while (length) {
        ssize_t count;
        if (files_wait_fd(fd, POLLIN) != 0) return -1;
        count = read(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return -1;
        }
    }
    return 0;
}

int files_write_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length) {
        ssize_t count;
        if (files_wait_fd(fd, POLLOUT) != 0) return -1;
        count = write(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return -1;
        }
    }
    return 0;
}

static int connect_tcp(const char *host, uint16_t port) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    socklen_t size = sizeof(int);
    int error = 0;
    int fd;
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        (fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    if ((connect(fd, (struct sockaddr *) &address, sizeof(address)) == 0 ||
         (errno == EINPROGRESS && files_wait_fd(fd, POLLOUT) == 0)) &&
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) {
        int enabled = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        return fd;
    }
    close(fd);
    return -1;
}

static int socks_reply(int fd) {
    uint8_t header[4];
    uint8_t address[256];
    uint8_t port[2];
    size_t length;
    if (files_read_all(fd, header, sizeof(header)) != 0 || header[0] != 5 || header[1] != 0)
        return -1;
    if (header[3] == 1) length = 4;
    else if (header[3] == 4) length = 16;
    else if (header[3] == 3) {
        if (files_read_all(fd, address, 1) != 0) return -1;
        length = address[0];
    } else return -1;
    return files_read_all(fd, address, length) == 0 && files_read_all(fd, port, sizeof(port)) == 0
           ? 0 : -1;
}

static int socks_connect(int fd, const char *host, uint16_t port) {
    uint8_t request[10] = {5, 1, 0, 1};
    uint16_t network_port = htons(port);
    uint8_t response[2];
    if (inet_pton(AF_INET, host, request + 4) != 1 || files_write_all(fd, "\x05\x01\x00", 3) != 0 ||
        files_read_all(fd, response, sizeof(response)) != 0 || response[0] != 5 || response[1] != 0)
        return -1;
    memcpy(request + 8, &network_port, sizeof(network_port));
    return files_write_all(fd, request, sizeof(request)) == 0 && socks_reply(fd) == 0 ? 0 : -1;
}

int files_remote_open(const struct files *files, uint8_t operation) {
    uint8_t hello[38] = {'P', 'D', 'S', 'F', FILE_VERSION};
    int fd = connect_tcp(files->socks_host, files->socks_port);
    if (fd < 0 || socks_connect(fd, files->host, files->port) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    memcpy(hello + 5, files->token, sizeof(files->token));
    hello[37] = operation;
    if (files_write_all(fd, hello, sizeof(hello)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int files_write_u16(int fd, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t) value};
    return files_write_all(fd, bytes, sizeof(bytes));
}

int files_read_u16(int fd, uint16_t *value) {
    uint8_t bytes[2];
    if (files_read_all(fd, bytes, sizeof(bytes)) != 0) return -1;
    *value = (uint16_t) bytes[0] << 8 | bytes[1];
    return 0;
}

int files_write_u64(int fd, uint64_t value) {
    uint8_t bytes[8];
    for (size_t index = 0; index != sizeof(bytes); ++index)
        bytes[index] = (uint8_t)(value >> (56 - index * 8));
    return files_write_all(fd, bytes, sizeof(bytes));
}

int files_read_u64(int fd, uint64_t *value) {
    uint8_t bytes[8];
    uint64_t result = 0;
    if (files_read_all(fd, bytes, sizeof(bytes)) != 0) return -1;
    for (size_t index = 0; index != sizeof(bytes); ++index) result = result << 8 | bytes[index];
    *value = result;
    return 0;
}

int files_write_text(int fd, const char *text) {
    size_t length = strlen(text);
    return length <= UINT16_MAX && files_write_u16(fd, (uint16_t) length) == 0 &&
           files_write_all(fd, text, length) == 0 ? 0 : -1;
}

int files_read_text(int fd, char *text, size_t size) {
    uint16_t length;
    if (files_read_u16(fd, &length) != 0 || length >= size || files_read_all(fd, text, length) != 0)
        return -1;
    text[length] = '\0';
    return 0;
}

int files_remote_status(int fd) {
    uint8_t status;
    return files_read_all(fd, &status, 1) == 0 && status == 0 ? 0 : -1;
}

int files_remote_directory(const struct files *files, const char *parent, const char *name,
                           char *created, size_t created_size) {
    int fd = files_remote_open(files, FILE_DIRECTORY);
    if (fd < 0 || files_write_text(fd, parent) != 0 || files_write_text(fd, name) != 0 ||
        files_remote_status(fd) != 0 || files_read_text(fd, created, created_size) != 0 ||
        !files_valid_name(created)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    return 0;
}