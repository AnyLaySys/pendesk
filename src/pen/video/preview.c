#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "preview.h"
#include "frames.h"
#include "files.h"
#include "io.h"
#include "stream.h"
#include <sys/eventfd.h>
#include <sys/un.h>
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
            999), .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
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

static int video_listener(void) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, STREAM_PATH, sizeof(STREAM_PATH));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(STREAM_PATH);
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_request(int listener_fd, char *action, size_t action_size) {
    char request[4096];
    size_t length = 0;
    int fd;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
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
        if (!strncmp(request, "GET /files/", 11)) {
            char *end = strchr(request + 11, ' ');
            size_t size = end ? (size_t)(end - request - 11) : 0;
            if (size && size < action_size) {
                memcpy(action, request + 11, size);
                action[size] = '\0';
                return fd;
            }
        }
        break;
    }
    close(fd);
    return -1;
}

static void notify(struct preview *preview) {
    uint64_t one = 1;
    ssize_t result = write(preview->wake, &one, sizeof(one));
    (void) result;
}

void preview_reset(struct preview *preview) {
    pthread_mutex_lock(&preview->mutex);
    preview->head = preview->count = 0;
    preview->waiting = true;
    pthread_mutex_unlock(&preview->mutex);
    atomic_store(&preview->keyframe, true);
    notify(preview);
}

static void disconnect(struct preview *preview, int *client) {
    if (*client >= 0) close(*client);
    *client = -1;
    pthread_mutex_lock(&preview->mutex);
    preview->connected = false;
    preview->count = 0;
    pthread_mutex_unlock(&preview->mutex);
}

static int write_video(int fd, const uint8_t *data, size_t length) {
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (length && alive) {
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count > 0) {
            data += count;
            length -= (size_t) count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - started.tv_sec) * 1000 +
                           (now.tv_nsec - started.tv_nsec) / 1000000;
            if (elapsed >= 50) return -1;
            struct pollfd event = {.fd = fd, .events = POLLOUT};
            if (poll(&event, 1, 10) >= 0) continue;
        }
        return -1;
    }
    return length ? -1 : 0;
}

static void *serve(void *argument) {
    struct preview *preview = argument;
    struct preview_frame frame = {0};
    int client = -1;
    while (alive) {
        struct pollfd events[] = {{.fd = preview->listener, .events = POLLIN},
                                  {.fd = preview->wake, .events = POLLIN},
                                  {.fd = client, .events = POLLIN},
                                  {.fd = preview->video_listener, .events = POLLIN}};
        if (poll(events, 4, 100) < 0) continue;
        if (events[2].revents & (POLLERR | POLLHUP)) disconnect(preview, &client);
        if (client >= 0 && events[2].revents & POLLIN) {
            char command[32];
            ssize_t count = recv(client, command, sizeof(command), MSG_DONTWAIT);
            if (count <= 0) disconnect(preview, &client);
            else preview_reset(preview);
        }
        if (events[0].revents & POLLIN) {
            char action[64];
            int fd = accept_request(preview->listener, action, sizeof(action));
            if (fd >= 0) {
                static const char response[] = "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                files_submit_sync(preview->files, action);
                io_write_all(fd, response, sizeof(response) - 1);
                close(fd);
            }
        }
        if (events[3].revents & POLLIN) {
            int fd = accept4(preview->video_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                disconnect(preview, &client);
                int capacity = 65536;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
                client = fd;
                pthread_mutex_lock(&preview->mutex);
                preview->connected = true;
                pthread_mutex_unlock(&preview->mutex);
                preview_reset(preview);
            }
        }
        if (events[1].revents & POLLIN) {
            uint64_t value;
            ssize_t result = read(preview->wake, &value, sizeof(value));
            (void) result;
        }
        while (client >= 0 && alive) {
            pthread_mutex_lock(&preview->mutex);
            bool available = preview->count > 0;
            if (available) {
                struct preview_frame spare = frame;
                frame = preview->queue[preview->head];
                preview->queue[preview->head] = spare;
                preview->head = (preview->head + 1) % 4;
                --preview->count;
            }
            pthread_mutex_unlock(&preview->mutex);
            if (!available) break;
            struct stream_packet packet = {.length = (uint32_t) frame.length, .keyframe = frame.keyframe, .timestamp = frame.timestamp};
            if (write_video(client, (const uint8_t *) &packet, sizeof(packet)) != 0 ||
                write_video(client, frame.data, frame.length) != 0)
                disconnect(preview, &client);
        }
    }
    disconnect(preview, &client);
    free(frame.data);
    return NULL;
}

int preview_open(struct preview *preview, const struct cfg *cfg) {
    *preview = (struct preview) {.mutex = PTHREAD_MUTEX_INITIALIZER, .listener = -1, .video_listener = -1, .wake = -1, .waiting = true};
    atomic_init(&preview->keyframe, true);
    preview->files = files_new(cfg->host, cfg->port, cfg->socks_host, cfg->socks_port, cfg->token);
    if (!preview->files || (preview->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) < 0 ||
        (preview->listener = listener()) < 0 || (preview->video_listener = video_listener()) < 0 ||
        pthread_create(&preview->thread, NULL, serve, preview) != 0) {
        if (preview->listener >= 0) close(preview->listener);
        if (preview->video_listener >= 0) close(preview->video_listener);
        if (preview->wake >= 0) close(preview->wake);
        files_free(preview->files);
        pthread_mutex_destroy(&preview->mutex);
        return -1;
    }
    preview->thread_started = true;
    return 0;
}

void preview_publish(struct preview *preview, struct video_frame *source) {
    pthread_mutex_lock(&preview->mutex);
    if (!preview->connected) goto done;
    if (preview->count == 4) {
        preview->count = preview->head = 0;
        preview->waiting = true;
        atomic_store(&preview->keyframe, true);
    }
    if (preview->waiting && !source->keyframe) goto done;
    preview->waiting = false;
    struct preview_frame *frame = &preview->queue[(preview->head + preview->count) % 4];
    struct preview_frame spare = *frame;
    *frame = (struct preview_frame) {.data = source->data, .capacity = source->capacity,
        .length = source->length, .timestamp = source->timestamp, .keyframe = source->keyframe};
    source->data = spare.data;
    source->capacity = spare.capacity;
    ++preview->count;
    done:
    pthread_mutex_unlock(&preview->mutex);
    notify(preview);
}

void preview_close(struct preview *preview) {
    notify(preview);
    if (preview->thread_started) pthread_join(preview->thread, NULL);
    if (preview->listener >= 0) close(preview->listener);
    if (preview->video_listener >= 0) close(preview->video_listener);
    unlink(STREAM_PATH);
    if (preview->wake >= 0) close(preview->wake);
    files_free(preview->files);
    for (unsigned int i = 0; i < 4; ++i) free(preview->queue[i].data);
    pthread_mutex_destroy(&preview->mutex);
}
