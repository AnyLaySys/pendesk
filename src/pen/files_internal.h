#ifndef PENDESK_FILES_INTERNAL_H
#define PENDESK_FILES_INTERNAL_H

#include "files.h"
#include "io.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    FILE_ENTRIES = 192,
    FILE_NAME = 256,
    FILE_PATH = 1024,
    FILE_VERSION = 3,
    FILE_LIST = 1,
    FILE_DOWNLOAD = 2,
    FILE_UPLOAD = 3,
    FILE_DIRECTORY = 4
};
enum {
    FILE_STATUS_NONE, FILE_STATUS_SELECTED, FILE_STATUS_SUCCESS, FILE_STATUS_FAILED,
    FILE_STATUS_TRANSFER
};
#define FILE_STATE "/tmp/pendesk-files.json"
#define FILE_STATE_TEMP "/tmp/.pendesk-files.json.tmp"

struct file_entry {
    char name[FILE_NAME];
    uint64_t size;
    uint64_t modified;
    bool directory;
    uint8_t state;
    uint8_t progress;
    uint64_t total;
    uint64_t done;
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
};

int files_wait_fd(int fd, short events);

int files_read_all(int fd, void *data, size_t length);

int files_write_all(int fd, const void *data, size_t length);

int files_remote_open(const struct files *files, uint8_t operation);

int files_write_u16(int fd, uint16_t value);

int files_read_u16(int fd, uint16_t *value);

int files_write_u64(int fd, uint64_t value);

int files_read_u64(int fd, uint64_t *value);

int files_write_text(int fd, const char *text);

int files_read_text(int fd, char *text, size_t size);

int files_remote_status(int fd);

int files_remote_directory(const struct files *files, const char *parent, const char *name,
                           char *created, size_t created_size);

bool files_valid_name(const char *name);

int files_path_child(char *path, size_t size, const char *name);

int files_remote_list(struct files *files, struct file_panel *panel);

int files_refresh_panel(struct files *files, bool pen);

int files_panel_select(struct files *files, bool pen, size_t row);

int files_panel_open(struct files *files, bool pen, size_t row);

int files_state_write(struct files *files);

int files_transfer(struct files *files, bool upload);

#endif
