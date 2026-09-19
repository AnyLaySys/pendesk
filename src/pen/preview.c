#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "preview.h"
#include "files.h"
#include "io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int listener(void) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(
            7194), .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    int enabled = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void notify(struct preview *preview) {
    uint8_t value = 0;
    if (preview->event[1] >= 0) {
        ssize_t count = write(preview->event[1], &value, sizeof(value));
        (void) count;
    }
}

static void drain(int fd) {
    uint8_t bytes[64];
    while (read(fd, bytes, sizeof(bytes)) > 0) {}
}

enum request {
    FRAME, ACTION
};

static int accept_request(int listener_fd, enum request *kind, char *action, size_t action_size) {
    char request[4096];
    size_t length = 0;
    int fd;
    struct timeval timeout = {.tv_sec = 3};
    if (!alive || (fd = accept(listener_fd, NULL, NULL)) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (length < sizeof(request) - 1 && alive) {
        ssize_t count = read(fd, request + length, sizeof(request) - 1 - length);
        if (count <= 0) break;
        length += (size_t) count;
        request[length] = '\0';
        if (!strstr(request, "\r\n\r\n")) continue;
        if (!strncmp(request, "GET /frame?", 11)) {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            *kind = FRAME;
            return fd;
        }
        if (!strncmp(request, "GET /files/", 11)) {
            char *end = strchr(request + 11, ' ');
            size_t size = end ? (size_t)(end - request - 11) : 0;
            if (size && size < action_size) {
                memcpy(action, request + 11, size);
                action[size] = '\0';
                *kind = ACTION;
                return fd;
            }
        }
        break;
    }
    close(fd);
    return -1;
}

static int send_frame(struct preview *preview, int fd, uint64_t *delivered) {
    int result = 0;
    pthread_mutex_lock(&preview->mutex);
    if (preview->length && *delivered != preview->sequence) {
        char header[192];
        int length = snprintf(header, sizeof(header),
                              "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nETag: \"%llu\"\r\nConnection: close\r\n\r\n",
                              preview->length, (unsigned long long) preview->sequence);
        result = -1;
        if (length > 0 && (size_t) length < sizeof(header) &&
            io_write_all(fd, header, (size_t) length) == 0 &&
            io_write_all(fd, preview->frame, preview->length) == 0) {
            *delivered = preview->sequence;
            result = 1;
        }
    }
    pthread_mutex_unlock(&preview->mutex);
    return result;
}

static void *serve(void *argument) {
    struct preview *preview = argument;
    uint64_t delivered = 0;
    int pending = -1;
    while (alive) {
        char action[64];
        enum request kind;
        struct pollfd events[2] = {{.fd = preview->listener, .events = POLLIN},
                                   {.fd = preview->event[0], .events = POLLIN}};
        int fd;
        if (pending >= 0 && send_frame(preview, pending, &delivered) != 0) {
            close(pending);
            pending = -1;
        }
        if (poll(events, 2, -1) <= 0) continue;
        if (events[1].revents & POLLIN) drain(preview->event[0]);
        if (!(events[0].revents & POLLIN) ||
            (fd = accept_request(preview->listener, &kind, action, sizeof(action))) < 0)
            continue;
        if (kind == ACTION) {
            static const char response[] = "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            files_action(preview->files, action);
            io_write_all(fd, response, sizeof(response) - 1);
            close(fd);
        } else if (pending >= 0) close(fd);
        else {
            pending = fd;
            if (send_frame(preview, pending, &delivered) != 0) {
                close(pending);
                pending = -1;
            }
        }
    }
    if (pending >= 0) close(pending);
    return NULL;
}

int preview_open(struct preview *preview, const struct cfg *cfg) {
    *preview = (struct preview) {.mutex = PTHREAD_MUTEX_INITIALIZER, .listener = -1, .event = {-1, -1}};
    preview->files = files_new(cfg->host, cfg->port, cfg->socks_host, cfg->socks_port, cfg->token);
    if (!preview->files || pipe2(preview->event, O_CLOEXEC | O_NONBLOCK) != 0 ||
        (preview->listener = listener()) < 0 ||
        pthread_create(&preview->thread, NULL, serve, preview) != 0) {
        if (preview->listener >= 0) close(preview->listener);
        if (preview->event[0] >= 0) close(preview->event[0]);
        if (preview->event[1] >= 0) close(preview->event[1]);
        files_free(preview->files);
        pthread_mutex_destroy(&preview->mutex);
        return -1;
    }
    preview->thread_started = true;
    return 0;
}

void preview_clear(struct preview *preview) {
    pthread_mutex_lock(&preview->mutex);
    preview->length = 0;
    pthread_mutex_unlock(&preview->mutex);
    notify(preview);
}

int preview_publish(struct preview *preview, const uint8_t *frame, size_t length) {
    int result = 0;
    pthread_mutex_lock(&preview->mutex);
    if (preview->capacity < length) {
        uint8_t *replacement = realloc(preview->frame, length);
        if (!replacement) result = -1;
        else {
            preview->frame = replacement;
            preview->capacity = length;
        }
    }
    if (!result) {
        memcpy(preview->frame, frame, length);
        preview->length = length;
        ++preview->sequence;
    }
    pthread_mutex_unlock(&preview->mutex);
    if (!result) notify(preview);
    return result;
}

void preview_close(struct preview *preview) {
    notify(preview);
    if (preview->thread_started) pthread_join(preview->thread, NULL);
    if (preview->listener >= 0) close(preview->listener);
    if (preview->event[0] >= 0) close(preview->event[0]);
    if (preview->event[1] >= 0) close(preview->event[1]);
    files_free(preview->files);
    free(preview->frame);
    pthread_mutex_destroy(&preview->mutex);
}
