#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "files.h"
#include "io.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    FILE_ENTRIES = 192,
    FILE_NAME = 256,
    FILE_PATH = 1024,
    FILE_VERSION = 2,
    FILE_LIST = 1,
    FILE_DOWNLOAD = 2,
    FILE_UPLOAD = 3,
    FILE_DIRECTORY = 4
};
enum {
    FILE_STATUS_NONE, FILE_STATUS_SELECTED, FILE_STATUS_SUCCESS, FILE_STATUS_FAILED
};
#define FILE_STATE "/tmp/pendesk-files.json"
#define FILE_STATE_TEMP "/tmp/.pendesk-files.json.tmp"
struct file_entry {
    char name[FILE_NAME];
    uint64_t size;
    bool directory;
    uint8_t state;
};
struct file_panel {
    char path[FILE_PATH];
    struct file_entry entries[FILE_ENTRIES];
    size_t count;
};
struct files {
    char host[256];
    char socks_host[256];
    uint16_t port;
    uint16_t socks_port;
    uint8_t token[32];
    pthread_mutex_t mutex;
    struct file_panel pen;
    struct file_panel windows;
    uint64_t total;
    uint64_t copied;
    uint8_t transfer;
    uint8_t progress;
};

static int wait_fd(int fd, short events) {
    struct pollfd pollfd = {.fd = fd, .events = events};
    while (alive) {
        int result = poll(&pollfd, 1, -1);
        if (result > 0) {
            return 0;
        }
        if (result < 0 && errno != EINTR) {
            return -1;
        }
    }
    errno = EINTR;
    return -1;
}

static int read_all(int fd, void *data, size_t length) {
    uint8_t *bytes = data;
    while (length) {
        ssize_t count;
        if (wait_fd(fd, POLLIN) != 0) {
            return -1;
        }
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

static int write_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = data;
    while (length) {
        ssize_t count;
        if (wait_fd(fd, POLLOUT) != 0) {
            return -1;
        }
        count = write(fd, bytes, length);
        if (count > 0) {
            bytes += count;
            length -= (size_t) count;
        } else if (count <= 0 &&
                   (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))) {
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
        (fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) {
        return -1;
    }
    if ((connect(fd, (struct sockaddr *) &address, sizeof(address)) == 0 ||
         (errno == EINPROGRESS && wait_fd(fd, POLLOUT) == 0)) &&
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
    if (read_all(fd, header, sizeof(header)) != 0 || header[0] != 5 || header[1] != 0) {
        return -1;
    }
    if (header[3] == 1) {
        length = 4;
    } else if (header[3] == 4) {
        length = 16;
    } else if (header[3] == 3) {
        if (read_all(fd, address, 1) != 0) {
            return -1;
        }
        length = address[0];
    } else {
        return -1;
    }
    return read_all(fd, address, length) == 0 && read_all(fd, port, sizeof(port)) == 0 ? 0 : -1;
}

static int socks_connect(int fd, const char *host, uint16_t port) {
    uint8_t request[10] = {5, 1, 0, 1};
    uint16_t network_port = htons(port);
    uint8_t response[2];
    if (inet_pton(AF_INET, host, request + 4) != 1 || write_all(fd, "\x05\x01\x00", 3) != 0 ||
        read_all(fd, response, sizeof(response)) != 0 || response[0] != 5 || response[1] != 0) {
        return -1;
    }
    memcpy(request + 8, &network_port, sizeof(network_port));
    return write_all(fd, request, sizeof(request)) == 0 && socks_reply(fd) == 0 ? 0 : -1;
}

static int remote_open(const struct files *files, uint8_t operation) {
    uint8_t hello[38] = {'P', 'D', 'S', 'F', FILE_VERSION};
    int fd = connect_tcp(files->socks_host, files->socks_port);
    if (fd < 0 || socks_connect(fd, files->host, files->port) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    memcpy(hello + 5, files->token, sizeof(files->token));
    hello[37] = operation;
    if (write_all(fd, hello, sizeof(hello)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_u16(int fd, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t) value};
    return write_all(fd, bytes, sizeof(bytes));
}

static int read_u16(int fd, uint16_t *value) {
    uint8_t bytes[2];
    if (read_all(fd, bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    *value = (uint16_t) bytes[0] << 8 | bytes[1];
    return 0;
}

static int write_u64(int fd, uint64_t value) {
    uint8_t bytes[8];
    for (size_t index = 0; index != sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)(value >> (56 - index * 8));
    }
    return write_all(fd, bytes, sizeof(bytes));
}

static int read_u64(int fd, uint64_t *value) {
    uint8_t bytes[8];
    uint64_t result = 0;
    if (read_all(fd, bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    for (size_t index = 0; index != sizeof(bytes); ++index) {
        result = result << 8 | bytes[index];
    }
    *value = result;
    return 0;
}

static int write_text(int fd, const char *text) {
    size_t length = strlen(text);
    return length <= UINT16_MAX && write_u16(fd, (uint16_t) length) == 0 &&
           write_all(fd, text, length) == 0 ? 0 : -1;
}

static int read_text(int fd, char *text, size_t size) {
    uint16_t length;
    if (read_u16(fd, &length) != 0 || length >= size || read_all(fd, text, length) != 0) {
        return -1;
    }
    text[length] = '\0';
    return 0;
}

static int remote_status(int fd) {
    uint8_t status;
    return read_all(fd, &status, 1) == 0 && status == 0 ? 0 : -1;
}

static bool valid_name(const char *name) {
    return *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/') &&
           !strchr(name, '\\');
}

static int
remote_directory(const struct files *files, const char *parent, const char *name, char *created,
                 size_t created_size) {
    int fd = remote_open(files, FILE_DIRECTORY);
    if (fd < 0 || write_text(fd, parent) != 0 || write_text(fd, name) != 0 ||
        remote_status(fd) != 0 || read_text(fd, created, created_size) != 0 ||
        !valid_name(created)) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    close(fd);
    return 0;
}

static int entry_compare(const void *left, const void *right) {
    const struct file_entry *a = left;
    const struct file_entry *b = right;
    if (a->directory != b->directory) {
        return a->directory ? -1 : 1;
    }
    return strcasecmp(a->name, b->name);
}

static void panel_clear(struct file_panel *panel) {
    panel->count = 0;
}

static int path_child(char *path, size_t size, const char *name) {
    size_t length = strlen(path);
    int result;
    if (!valid_name(name)) {
        return -1;
    }
    if (!strcmp(path, "/")) {
        result = snprintf(path + 1, size - 1, "%s", name);
        return result >= 0 && (size_t) result < size - 1 ? 0 : -1;
    }
    result = length ? snprintf(path + length, size - length, "/%s", name) : snprintf(path, size,
                                                                                     "%s", name);
    return result >= 0 && (size_t) result < (length ? size - length : size) ? 0 : -1;
}

static void path_parent(char *path) {
    char *separator = strrchr(path, '/');
    if (!strcmp(path, "/") || !*path) {
        return;
    }
    if (separator) {
        if (separator == path) {
            path[1] = '\0';
        } else {
            *separator = '\0';
        }
    } else {
        path[0] = '\0';
    }
}

static bool panel_parent(const struct file_panel *panel) {
    return panel->path[0] && strcmp(panel->path, "/");
}

static int local_list(struct file_panel *panel) {
    DIR *directory;
    struct dirent *entry;
    if (!(directory = opendir(panel->path))) {
        return -1;
    }
    panel_clear(panel);
    while ((entry = readdir(directory))) {
        struct stat status;
        char full[FILE_PATH + FILE_NAME + 32];
        struct file_entry *output;
        int length;
        if (!valid_name(entry->d_name) || panel->count == FILE_ENTRIES) {
            continue;
        }
        length = snprintf(full, sizeof(full), "%s%s%s", panel->path,
                          !strcmp(panel->path, "/") ? "" : "/", entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(full) || lstat(full, &status) != 0 ||
            (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))) {
            continue;
        }
        output = &panel->entries[panel->count++];
        snprintf(output->name, sizeof(output->name), "%s", entry->d_name);
        output->directory = S_ISDIR(status.st_mode);
        output->size = (uint64_t) status.st_size;
        output->state = FILE_STATUS_NONE;
    }
    closedir(directory);
    qsort(panel->entries, panel->count, sizeof(panel->entries[0]), entry_compare);
    return 0;
}

static int remote_list(struct files *files, struct file_panel *panel) {
    uint16_t count;
    int fd = remote_open(files, FILE_LIST);
    if (fd < 0 || write_text(fd, panel->path) != 0 || remote_status(fd) != 0 ||
        read_u16(fd, &count) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    panel_clear(panel);
    for (uint16_t index = 0; index != count; ++index) {
        uint8_t kind;
        struct file_entry item;
        if (read_all(fd, &kind, 1) != 0 || read_text(fd, item.name, sizeof(item.name)) != 0 ||
            read_u64(fd, &item.size) != 0 || (kind != 0 && kind != 1) || !valid_name(item.name)) {
            close(fd);
            return -1;
        }
        item.directory = kind != 0;
        item.state = FILE_STATUS_NONE;
        if (panel->count != FILE_ENTRIES) {
            panel->entries[panel->count++] = item;
        }
    }
    close(fd);
    return 0;
}

static int refresh_panel(struct files *files, bool pen) {
    return pen ? local_list(&files->pen) : remote_list(files, &files->windows);
}

static void refresh_all(struct files *files) {
    refresh_panel(files, true);
    refresh_panel(files, false);
}

static int panel_select(struct files *files, bool pen, size_t row) {
    struct file_panel *panel = pen ? &files->pen : &files->windows;
    size_t index;
    bool parent = panel_parent(panel);
    if (parent && !row) {
        return -1;
    }
    index = row - parent;
    if (index >= panel->count) {
        return -1;
    }
    panel->entries[index].state =
            panel->entries[index].state == FILE_STATUS_SELECTED ? FILE_STATUS_NONE
                                                                : FILE_STATUS_SELECTED;
    return 0;
}

static int panel_open(struct files *files, bool pen, size_t row) {
    struct file_panel *panel = pen ? &files->pen : &files->windows;
    size_t index;
    bool parent = panel_parent(panel);
    if (parent && !row) {
        path_parent(panel->path);
        return refresh_panel(files, pen);
    }
    index = row - parent;
    if (index >= panel->count) {
        return -1;
    }
    return panel->entries[index].directory &&
           path_child(panel->path, sizeof(panel->path), panel->entries[index].name) == 0 &&
           refresh_panel(files, pen) == 0 ? 0 : -1;
}

static int state_text(FILE *output, const char *text) {
    const uint8_t *input = (const uint8_t *) text;
    if (fputc('"', output) == EOF) {
        return -1;
    }
    while (*input) {
        uint32_t value = *input++;
        if (value >= 0x80) {
            if (value >= 0xc2 && value <= 0xdf && input[0] && (input[0] & 0xc0) == 0x80) {
                value = (value & 0x1f) << 6 | (*input++ & 0x3f);
            } else if (value >= 0xe0 && value <= 0xef && input[0] && input[1] &&
                       (input[0] & 0xc0) == 0x80 && (input[1] & 0xc0) == 0x80) {
                value = (value & 0x0f) << 12 | (input[0] & 0x3f) << 6 | (input[1] & 0x3f);
                input += 2;
            } else if (value >= 0xf0 && value <= 0xf4 && input[0] && input[1] && input[2] &&
                       (input[0] & 0xc0) == 0x80 && (input[1] & 0xc0) == 0x80 &&
                       (input[2] & 0xc0) == 0x80) {
                value = (value & 0x07) << 18 | (input[0] & 0x3f) << 12 | (input[1] & 0x3f) << 6 |
                        (input[2] & 0x3f);
                input += 3;
            } else {
                value = '?';
            }
        }
        if (value >= 0x20 && value <= 0x7e && value != '"' && value != '\\') {
            if (fputc((int) value, output) == EOF) {
                return -1;
            }
        } else if (value <= 0xffff) {
            if (fprintf(output, "\\u%04x", value) < 0) {
                return -1;
            }
        } else if (fprintf(output, "\\u%04x\\u%04x", 0xd800 + ((value - 0x10000) >> 10),
                           0xdc00 + ((value - 0x10000) & 0x3ff)) < 0) {
            return -1;
        }
    }
    return fputc('"', output) == EOF ? -1 : 0;
}

static int state_entry(FILE *output, const struct file_entry *entry, bool parent) {
    if (fputs("{\"name\":", output) == EOF ||
        state_text(output, parent ? ".." : entry->name) != 0 ||
        fprintf(output, ",\"size\":%llu,\"directory\":%u,\"state\":%u,\"parent\":%u}",
                (unsigned long long) (parent ? 0 : entry->size), parent || entry->directory,
                parent ? FILE_STATUS_NONE : entry->state, parent) < 0) {
        return -1;
    }
    return 0;
}

static int state_panel(FILE *output, const struct file_panel *panel) {
    bool comma = false;
    if (fputc('[', output) == EOF) {
        return -1;
    }
    if (panel_parent(panel)) {
        if (state_entry(output, NULL, true) != 0) {
            return -1;
        }
        comma = true;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        if (comma && fputc(',', output) == EOF) {
            return -1;
        }
        if (state_entry(output, &panel->entries[index], false) != 0) {
            return -1;
        }
        comma = true;
    }
    return fputc(']', output) == EOF ? -1 : 0;
}

static int state_write(const struct files *files) {
    FILE *output = fopen(FILE_STATE_TEMP, "w");
    int result = -1;
    if (!output) {
        return -1;
    }
    if (fputs("{\"pen\":", output) != EOF && state_panel(output, &files->pen) == 0 &&
        fputs(",\"windows\":", output) != EOF && state_panel(output, &files->windows) == 0 &&
        fprintf(output, ",\"transfer\":%u,\"progress\":%u}", (unsigned int) files->transfer,
                (unsigned int) files->progress) >= 0) {
        result = 0;
    }
    if (fclose(output) != 0) {
        result = -1;
    }
    if (result != 0) {
        unlink(FILE_STATE_TEMP);
        return -1;
    }
    return rename(FILE_STATE_TEMP, FILE_STATE);
}

static void transfer_progress(struct files *files, uint64_t count) {
    unsigned int progress;
    files->copied += count;
    progress = files->copied >= files->total ? 100 : (unsigned int) (files->copied * 100 /
                                                                     files->total);
    if (progress != files->progress) {
        files->progress = progress;
        state_write(files);
    }
}

static int file_copy_to_fd(struct files *files, int input, int output, uint64_t length) {
    uint8_t buffer[65536];
    while (length) {
        size_t block = length > sizeof(buffer) ? sizeof(buffer) : (size_t) length;
        ssize_t count = read(input, buffer, block);
        if (count <= 0 || write_all(output, buffer, (size_t) count) != 0) {
            return -1;
        }
        length -= (size_t) count;
        transfer_progress(files, (uint64_t) count);
    }
    return 0;
}

static int file_copy_from_fd(struct files *files, int input, int output, uint64_t length) {
    uint8_t buffer[65536];
    while (length) {
        size_t block = length > sizeof(buffer) ? sizeof(buffer) : (size_t) length;
        if (read_all(input, buffer, block) != 0) {
            return -1;
        }
        if (write_all(output, buffer, block) != 0) {
            return -1;
        }
        length -= block;
        transfer_progress(files, block);
    }
    return 0;
}

static int
pen_destination(const char *directory, const char *name, char *target, size_t target_size,
                char *temporary, size_t temporary_size) {
    int length;
    if (!valid_name(name)) {
        return -1;
    }
    for (unsigned int index = 0; index != 10000; ++index) {
        length = index ? snprintf(target, target_size, "%s/%s (%u)", directory, name, index)
                       : snprintf(target, target_size, "%s/%s", directory, name);
        if (length < 0 || (size_t) length >= target_size) {
            return -1;
        }
        if (access(target, F_OK) != 0 && errno == ENOENT) {
            length = snprintf(temporary, temporary_size, "%s/.pendesk-%ld-%u.tmp", directory,
                              (long) getpid(), index);
            return length >= 0 && (size_t) length < temporary_size ? 0 : -1;
        }
    }
    return -1;
}

static int pen_directory(const char *parent, const char *name, char *path, size_t size) {
    int length;
    if (!valid_name(name)) {
        return -1;
    }
    for (unsigned int index = 0; index != 10000; ++index) {
        length = index ? snprintf(path, size, "%s/%s (%u)", parent, name, index) : snprintf(path,
                                                                                            size,
                                                                                            "%s/%s",
                                                                                            parent,
                                                                                            name);
        if (length < 0 || (size_t) length >= size) {
            return -1;
        }
        if (mkdir(path, 0700) == 0) {
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
}

static int total_add(uint64_t *total, uint64_t size) {
    if (UINT64_MAX - *total < size) {
        return -1;
    }
    *total += size;
    return 0;
}

static int pen_total(const char *path, unsigned int depth, uint64_t *total) {
    struct stat status;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &status) != 0) {
        return -1;
    }
    if (S_ISREG(status.st_mode)) return status.st_size < 0 ? -1 : total_add(total, (uint64_t) status.st_size);
    if (!S_ISDIR(status.st_mode)) {
        return 0;
    }
    if (!(directory = opendir(path))) {
        return -1;
    }
    while ((entry = readdir(directory))) {
        char child[FILE_PATH + FILE_NAME + 32];
        if (!valid_name(entry->d_name)) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s", path) < 0 || strlen(path) >= sizeof(child) ||
            path_child(child, sizeof(child), entry->d_name) != 0 ||
            pen_total(child, depth + 1, total) != 0) {
            closedir(directory);
            return -1;
        }
    }
    return closedir(directory) == 0 ? 0 : -1;
}

static int
remote_total(struct files *files, const char *source, unsigned int depth, uint64_t *total) {
    struct file_panel *panel = calloc(1, sizeof(*panel));
    int result = -1;
    if (!panel ||
        snprintf(panel->path, sizeof(panel->path), "%s", source) < 0 ||
        strlen(source) >= sizeof(panel->path) || remote_list(files, panel) != 0) {
        free(panel);
        return -1;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        char child[FILE_PATH + FILE_NAME + 2];
        if (snprintf(child, sizeof(child), "%s", source) < 0 || strlen(source) >= sizeof(child) ||
            path_child(child, sizeof(child), panel->entries[index].name) != 0 ||
            (panel->entries[index].directory ? remote_total(files, child, depth + 1, total)
                                             : total_add(total, panel->entries[index].size)) != 0) {
            free(panel);
            return -1;
        }
    }
    result = 0;
    free(panel);
    return result;
}

static int upload_total(struct files *files, const struct file_entry *entry, uint64_t *total) {
    char path[FILE_PATH + FILE_NAME + 32];
    if (!entry || snprintf(path, sizeof(path), "%s", files->pen.path) < 0 ||
        strlen(files->pen.path) >= sizeof(path) ||
        path_child(path, sizeof(path), entry->name) != 0) {
        return -1;
    }
    return pen_total(path, 0, total);
}

static int download_total(struct files *files, const struct file_entry *entry, uint64_t *total) {
    char source[FILE_PATH + FILE_NAME + 2];
    if (!entry || snprintf(source, sizeof(source), "%s", files->windows.path) < 0 ||
        strlen(files->windows.path) >= sizeof(source) ||
        path_child(source, sizeof(source), entry->name) != 0) {
        return -1;
    }
    return entry->directory ? remote_total(files, source, 0, total) : total_add(total, entry->size);
}

static int transfer_total(struct files *files, bool upload) {
    struct file_panel *source = upload ? &files->pen : &files->windows;
    for (size_t index = 0; index != source->count; ++index) {
        if (source->entries[index].state == FILE_STATUS_SELECTED &&
            (upload ? upload_total(files, &source->entries[index], &files->total) : download_total(
                    files, &source->entries[index], &files->total)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int download_file(struct files *files, const char *source, const char *directory) {
    char name[FILE_NAME];
    char target[FILE_PATH + FILE_NAME + 64];
    char temporary[FILE_PATH + 64];
    uint64_t length;
    int fd = -1;
    int output = -1;
    if ((fd = remote_open(files, FILE_DOWNLOAD)) < 0 || write_text(fd, source) != 0 ||
        remote_status(fd) != 0 || read_text(fd, name, sizeof(name)) != 0 ||
        read_u64(fd, &length) != 0 ||
        pen_destination(directory, name, target, sizeof(target), temporary, sizeof(temporary)) !=
        0 || (output = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    if (file_copy_from_fd(files, fd, output, length) != 0 || fsync(output) != 0) {
        close(fd);
        close(output);
        unlink(temporary);
        return -1;
    }
    if (close(output) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    output = -1;
    if (rename(temporary, target) != 0) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    close(fd);
    return 0;
}

static int
download_directory(struct files *files, const char *source, const char *parent, const char *name,
                   unsigned int depth) {
    struct file_panel *panel = calloc(1, sizeof(*panel));
    char directory[FILE_PATH + FILE_NAME + 64];
    int result = -1;
    if (!panel ||
        pen_directory(parent, name, directory, sizeof(directory)) != 0 ||
        snprintf(panel->path, sizeof(panel->path), "%s", source) < 0 ||
        strlen(source) >= sizeof(panel->path) || remote_list(files, panel) != 0) {
        free(panel);
        return -1;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        char child[FILE_PATH + FILE_NAME + 2];
        if (snprintf(child, sizeof(child), "%s", source) < 0 || strlen(source) >= sizeof(child) ||
            path_child(child, sizeof(child), panel->entries[index].name) != 0 ||
            (panel->entries[index].directory ? download_directory(files, child, directory,
                                                                  panel->entries[index].name,
                                                                  depth + 1) : download_file(files,
                                                                                             child,
                                                                                             directory)) !=
            0) {
            free(panel);
            return -1;
        }
    }
    result = 0;
    free(panel);
    return result;
}

static int download_to_pen(struct files *files, const struct file_entry *entry) {
    char source[FILE_PATH + FILE_NAME + 2];
    char directory[FILE_PATH + 32];
    if (!entry || snprintf(directory, sizeof(directory), "%s", files->pen.path) < 0 ||
        strlen(files->pen.path) >= sizeof(directory) ||
        snprintf(source, sizeof(source), "%s", files->windows.path) < 0 ||
        strlen(files->windows.path) >= sizeof(source) ||
        path_child(source, sizeof(source), entry->name) != 0) {
        return -1;
    }
    return entry->directory ? download_directory(files, source, directory, entry->name, 0)
                            : download_file(files, source, directory);
}

static int upload_file(struct files *files, int fd, const char *path, const char *directory,
                       const char *name) {
    struct stat status;
    int input = -1;
    if (!valid_name(name) || (input = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0 ||
        fstat(input, &status) != 0 || !S_ISREG(status.st_mode) ||
        write_text(fd, directory) != 0 ||
        write_text(fd, name) != 0 ||
        write_u64(fd, (uint64_t) status.st_size) != 0) {
        if (input >= 0) {
            close(input);
        }
        return -1;
    }
    if (file_copy_to_fd(files, input, fd, (uint64_t) status.st_size) != 0 ||
        remote_status(fd) != 0 || remote_status(fd) != 0) {
        close(input);
        return -1;
    }
    close(input);
    return 0;
}

static int upload_directories(struct files *files, const char *path, const char *parent,
                              const char *name, char *target, size_t target_size) {
    char created[FILE_NAME];
    char directory[FILE_PATH];
    DIR *input;
    struct dirent *entry;
    if (remote_directory(files, parent, name, created, sizeof(created)) != 0 ||
        snprintf(directory, sizeof(directory), "%s", parent) < 0 ||
        strlen(parent) >= sizeof(directory) ||
        path_child(directory, sizeof(directory), created) != 0 ||
        (target && (snprintf(target, target_size, "%s", directory) < 0 ||
                    strlen(directory) >= target_size)) || !(input = opendir(path))) {
        return -1;
    }
    while ((entry = readdir(input))) {
        struct stat status;
        char child[FILE_PATH + FILE_NAME + 32];
        int length;
        if (!valid_name(entry->d_name)) {
            continue;
        }
        length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(child) || lstat(child, &status) != 0) {
            closedir(input);
            return -1;
        }
        if (S_ISDIR(status.st_mode)) {
            if (upload_directories(files, child, directory, entry->d_name, NULL, 0) != 0) {
                closedir(input);
                return -1;
            }
        }
    }
    return closedir(input) == 0 ? 0 : -1;
}

static int upload_tree(struct files *files, int fd, const char *path, const char *directory) {
    DIR *input;
    struct dirent *entry;
    if (!(input = opendir(path))) {
        return -1;
    }
    while ((entry = readdir(input))) {
        struct stat status;
        char child[FILE_PATH + FILE_NAME + 32];
        char target[FILE_PATH];
        int length;
        if (!valid_name(entry->d_name)) {
            continue;
        }
        length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(child) || lstat(child, &status) != 0) {
            closedir(input);
            return -1;
        }
        if (S_ISDIR(status.st_mode)) {
            if (snprintf(target, sizeof(target), "%s", directory) < 0 ||
                strlen(directory) >= sizeof(target) ||
                path_child(target, sizeof(target), entry->d_name) != 0 ||
                upload_tree(files, fd, child, target) != 0) {
                closedir(input);
                return -1;
            }
        } else if (S_ISREG(status.st_mode) &&
                   upload_file(files, fd, child, directory, entry->d_name) != 0) {
            closedir(input);
            return -1;
        }
    }
    return closedir(input) == 0 ? 0 : -1;
}

static int upload_from_pen(struct files *files, const struct file_entry *entry) {
    char path[FILE_PATH + FILE_NAME + 32];
    char directory[FILE_PATH];
    int fd;
    int result;
    if (!entry || snprintf(path, sizeof(path), "%s", files->pen.path) < 0 ||
        strlen(files->pen.path) >= sizeof(path) ||
        path_child(path, sizeof(path), entry->name) != 0) {
        return -1;
    }
    if (entry->directory &&
        upload_directories(files, path, files->windows.path, entry->name, directory,
                           sizeof(directory)) != 0) {
        return -1;
    }
    fd = remote_open(files, FILE_UPLOAD);
    if (fd < 0) return -1;
    result = entry->directory ? upload_tree(files, fd, path, directory)
                              : upload_file(files, fd, path, files->windows.path, entry->name);
    close(fd);
    return result;
}

static int transfer(struct files *files, bool upload) {
    struct file_panel *source = upload ? &files->pen : &files->windows;
    int result = 0;
    size_t completed = 0;
    files->total = 0;
    files->copied = 0;
    files->progress = 0;
    files->transfer = upload ? 1 : 2;
    state_write(files);
    if (transfer_total(files, upload) != 0) {
        files->transfer = 0;
        return -1;
    }
    for (size_t index = 0; index != source->count; ++index) {
        if (source->entries[index].state != FILE_STATUS_SELECTED) {
            continue;
        }
        if ((upload ? upload_from_pen(files, &source->entries[index]) : download_to_pen(files,
                                                                                        &source->entries[index])) !=
            0) {
            source->entries[index].state = FILE_STATUS_FAILED;
            result = -1;
            break;
        }
        source->entries[index].state = FILE_STATUS_SUCCESS;
        ++completed;
    }
    files->transfer = 0;
    if (!completed || refresh_panel(files, !upload) != 0) {
        return -1;
    }
    return result;
}

static bool side(const char *text, bool *pen) {
    if (!strcmp(text, "pen")) {
        *pen = true;
        return true;
    }
    if (!strcmp(text, "windows")) {
        *pen = false;
        return true;
    }
    return false;
}

static int action_row(struct files *files, const char *text, bool open) {
    char value[32];
    char *end;
    bool pen;
    unsigned long row;
    if (strlen(text) >= sizeof(value)) {
        return -1;
    }
    snprintf(value, sizeof(value), "%s", text);
    end = strrchr(value, '/');
    if (!end) {
        return -1;
    }
    *end++ = '\0';
    if (!side(value, &pen) || !*end) {
        return -1;
    }
    row = strtoul(end, &end, 10);
    return *end || row > SIZE_MAX ? -1 : (open ? panel_open(files, pen, (size_t) row)
                                               : panel_select(files, pen, (size_t) row));
}

struct files *
files_new(const char *host, uint16_t port, const char *socks_host, uint16_t socks_port,
          const uint8_t token[32]) {
    struct files *files;
    if (!host || !socks_host || !token) {
        return NULL;
    }
    files = calloc(1, sizeof(*files));
    if (!files || strlen(host) >= sizeof(files->host) ||
        strlen(socks_host) >= sizeof(files->socks_host)) {
        free(files);
        return NULL;
    }
    snprintf(files->host, sizeof(files->host), "%s", host);
    snprintf(files->socks_host, sizeof(files->socks_host), "%s", socks_host);
    files->port = port;
    files->socks_port = socks_port;
    memcpy(files->token, token, sizeof(files->token));
    if (pthread_mutex_init(&files->mutex, NULL) != 0) {
        free(files);
        return NULL;
    }
    files->pen.path[0] = '/';
    return files;
}

void files_free(struct files *files) {
    if (files) {
        pthread_mutex_destroy(&files->mutex);
        free(files);
    }
}

int files_action(struct files *files, const char *action) {
    int result = -1;
    if (!files || !action) {
        return -1;
    }
    pthread_mutex_lock(&files->mutex);
    if (!strcmp(action, "reset")) {
        files->pen.path[0] = '/';
        files->pen.path[1] = '\0';
        files->windows.path[0] = '\0';
        refresh_all(files);
        result = 0;
    } else if (!strncmp(action, "open/", 5)) {
        result = action_row(files, action + 5, true);
    } else if (!strncmp(action, "select/", 7)) {
        result = action_row(files, action + 7, false);
    } else if (!strcmp(action, "transfer/push")) {
        result = transfer(files, true);
    } else if (!strcmp(action, "transfer/pull")) {
        result = transfer(files, false);
    }
    state_write(files);
    pthread_mutex_unlock(&files->mutex);
    return result;
}
