#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "files_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void transfer_progress(struct files *files, struct file_entry *entry, uint64_t bytes) {
    entry->done += bytes;
    uint8_t progress = entry->total ? (uint8_t) (entry->done >= entry->total ? 100 : entry->done * 100 / entry->total) : 0;
    if (progress != entry->progress) {
        entry->progress = progress;
        files_state_write(files);
    }
}

static int file_copy_to_fd(struct files *files, struct file_entry *entry, int input, int output,
                           uint64_t length) {
    uint8_t buffer[65536];
    while (length) {
        size_t block = length > sizeof(buffer) ? sizeof(buffer) : (size_t) length;
        ssize_t count = read(input, buffer, block);
        if (count <= 0 || files_write_all(output, buffer, (size_t) count) != 0) return -1;
        length -= (size_t) count;
        transfer_progress(files, entry, (uint64_t) count);
    }
    return 0;
}

static int file_copy_from_fd(struct files *files, struct file_entry *entry, int input, int output,
                             uint64_t length) {
    uint8_t buffer[65536];
    while (length) {
        size_t block = length > sizeof(buffer) ? sizeof(buffer) : (size_t) length;
        if (files_read_all(input, buffer, block) != 0 ||
            files_write_all(output, buffer, block) != 0)
            return -1;
        length -= block;
        transfer_progress(files, entry, block);
    }
    return 0;
}

static int
pen_destination(const char *directory, const char *name, char *target, size_t target_size,
                char *temporary, size_t temporary_size) {
    int length;
    if (!files_valid_name(name)) return -1;
    for (unsigned int index = 0; index != 10000; ++index) {
        length = index ? snprintf(target, target_size, "%s/%s (%u)", directory, name, index)
                       : snprintf(target, target_size, "%s/%s", directory, name);
        if (length < 0 || (size_t) length >= target_size) return -1;
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
    if (!files_valid_name(name)) return -1;
    for (unsigned int index = 0; index != 10000; ++index) {
        length = index ? snprintf(path, size, "%s/%s (%u)", parent, name, index) : snprintf(path,
                                                                                            size,
                                                                                            "%s/%s",
                                                                                            parent,
                                                                                            name);
        if (length < 0 || (size_t) length >= size) return -1;
        if (mkdir(path, 0700) == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

static int add_total(uint64_t *total, uint64_t size) {
    if (UINT64_MAX - *total < size) return -1;
    *total += size;
    return 0;
}

static int pen_total(const char *path, uint64_t *total) {
    struct stat status;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &status) != 0) return -1;
    if (S_ISREG(status.st_mode))
        return status.st_size < 0 ? -1 : add_total(total, (uint64_t) status.st_size);
    if (!S_ISDIR(status.st_mode)) return 0;
    if (!(directory = opendir(path))) return -1;
    while ((entry = readdir(directory))) {
        char child[FILE_PATH + FILE_NAME + 32];
        if (!files_valid_name(entry->d_name)) continue;
        if (snprintf(child, sizeof(child), "%s", path) < 0 || strlen(path) >= sizeof(child) ||
            files_path_child(child, sizeof(child), entry->d_name) != 0 || pen_total(child, total)) {
            closedir(directory);
            return -1;
        }
    }
    return closedir(directory) == 0 ? 0 : -1;
}

static int remote_total(struct files *files, const char *path, uint64_t *total) {
    struct file_panel *panel = calloc(1, sizeof(*panel));
    if (!panel || snprintf(panel->path, sizeof(panel->path), "%s", path) < 0 ||
        strlen(path) >= sizeof(panel->path) || files_remote_list(files, panel)) {
        free(panel);
        return -1;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        char child[FILE_PATH + FILE_NAME + 2];
        if (snprintf(child, sizeof(child), "%s", path) < 0 || strlen(path) >= sizeof(child) ||
            files_path_child(child, sizeof(child), panel->entries[index].name) != 0 ||
            (panel->entries[index].directory ? remote_total(files, child, total)
                                             : add_total(total, panel->entries[index].size))) {
            free(panel);
            return -1;
        }
    }
    free(panel);
    return 0;
}

static int entry_total(struct files *files, bool upload, const struct file_entry *entry,
                       uint64_t *total) {
    char path[FILE_PATH + FILE_NAME + 32];
    const char *parent = upload ? files->pen.path : files->windows.path;
    if (snprintf(path, sizeof(path), "%s", parent) < 0 || strlen(parent) >= sizeof(path) ||
        files_path_child(path, sizeof(path), entry->name) != 0)
        return -1;
    if (!entry->directory) return add_total(total, entry->size);
    return upload ? pen_total(path, total) : remote_total(files, path, total);
}

static int download_file(struct files *files, struct file_entry *entry, const char *source,
                         const char *directory) {
    char name[FILE_NAME];
    char target[FILE_PATH + FILE_NAME + 64];
    char temporary[FILE_PATH + 64];
    uint64_t length;
    int fd = -1;
    int output = -1;
    if ((fd = files_remote_open(files, FILE_DOWNLOAD)) < 0 || files_write_text(fd, source) != 0 ||
        files_remote_status(fd) != 0 || files_read_text(fd, name, sizeof(name)) != 0 ||
        files_read_u64(fd, &length) != 0 ||
        pen_destination(directory, name, target, sizeof(target), temporary, sizeof(temporary)) !=
        0 || (output = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (file_copy_from_fd(files, entry, fd, output, length) != 0 || fsync(output) != 0) {
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

static int download_directory(struct files *files, struct file_entry *entry, const char *source,
                              const char *parent, const char *name) {
    struct file_panel *panel = calloc(1, sizeof(*panel));
    char directory[FILE_PATH + FILE_NAME + 64];
    int result = -1;
    if (!panel || pen_directory(parent, name, directory, sizeof(directory)) != 0 ||
        snprintf(panel->path, sizeof(panel->path), "%s", source) < 0 ||
        strlen(source) >= sizeof(panel->path) || files_remote_list(files, panel) != 0) {
        free(panel);
        return -1;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        char child[FILE_PATH + FILE_NAME + 2];
        if (snprintf(child, sizeof(child), "%s", source) < 0 || strlen(source) >= sizeof(child) ||
            files_path_child(child, sizeof(child), panel->entries[index].name) != 0 ||
            (panel->entries[index].directory ? download_directory(files, entry, child, directory,
                                                                  panel->entries[index].name)
                                             : download_file(files, entry, child, directory)) != 0) {
            free(panel);
            return -1;
        }
    }
    result = 0;
    free(panel);
    return result;
}

static int download_to_pen(struct files *files, struct file_entry *entry) {
    char source[FILE_PATH + FILE_NAME + 2];
    char directory[FILE_PATH + 32];
    if (!entry || snprintf(directory, sizeof(directory), "%s", files->pen.path) < 0 ||
        strlen(files->pen.path) >= sizeof(directory) ||
        snprintf(source, sizeof(source), "%s", files->windows.path) < 0 ||
        strlen(files->windows.path) >= sizeof(source) ||
        files_path_child(source, sizeof(source), entry->name) != 0)
        return -1;
    return entry->directory ? download_directory(files, entry, source, directory, entry->name)
                            : download_file(files, entry, source, directory);
}

static int upload_file(struct files *files, struct file_entry *entry, int fd, const char *path,
                       const char *directory, const char *name) {
    struct stat status;
    int input = -1;
    if (!files_valid_name(name) || (input = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0 ||
        fstat(input, &status) != 0 || !S_ISREG(status.st_mode) ||
        files_write_text(fd, directory) != 0 || files_write_text(fd, name) != 0 ||
        files_write_u64(fd, (uint64_t) status.st_size) != 0) {
        if (input >= 0) close(input);
        return -1;
    }
    if (file_copy_to_fd(files, entry, input, fd, (uint64_t) status.st_size) != 0 ||
        files_remote_status(fd) != 0 || files_remote_status(fd) != 0) {
        close(input);
        return -1;
    }
    close(input);
    return 0;
}

static int
upload_directories(struct files *files, const char *path, const char *parent, const char *name,
                   char *target, size_t target_size) {
    char created[FILE_NAME];
    char directory[FILE_PATH];
    DIR *input;
    struct dirent *entry;
    if (files_remote_directory(files, parent, name, created, sizeof(created)) != 0 ||
        snprintf(directory, sizeof(directory), "%s", parent) < 0 ||
        strlen(parent) >= sizeof(directory) ||
        files_path_child(directory, sizeof(directory), created) != 0 || (target && (snprintf(target,
                                                                                             target_size,
                                                                                             "%s",
                                                                                             directory) <
                                                                                    0 ||
                                                                                    strlen(directory) >=
                                                                                    target_size)) ||
        !(input = opendir(path)))
        return -1;
    while ((entry = readdir(input))) {
        struct stat status;
        char child[FILE_PATH + FILE_NAME + 32];
        int length;
        if (!files_valid_name(entry->d_name)) continue;
        length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(child) || lstat(child, &status) != 0) {
            closedir(input);
            return -1;
        }
        if (S_ISDIR(status.st_mode) &&
            upload_directories(files, child, directory, entry->d_name, NULL, 0) != 0) {
            closedir(input);
            return -1;
        }
    }
    return closedir(input) == 0 ? 0 : -1;
}

static int upload_tree(struct files *files, struct file_entry *progress_entry, int fd, const char *path,
                       const char *directory) {
    DIR *input;
    struct dirent *entry;
    if (!(input = opendir(path))) return -1;
    while ((entry = readdir(input))) {
        struct stat status;
        char child[FILE_PATH + FILE_NAME + 32];
        char target[FILE_PATH];
        int length;
        if (!files_valid_name(entry->d_name)) continue;
        length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(child) || lstat(child, &status) != 0) {
            closedir(input);
            return -1;
        }
        if (S_ISDIR(status.st_mode)) {
            if (snprintf(target, sizeof(target), "%s", directory) < 0 ||
                strlen(directory) >= sizeof(target) ||
                files_path_child(target, sizeof(target), entry->d_name) != 0 ||
                upload_tree(files, progress_entry, fd, child, target) != 0) {
                closedir(input);
                return -1;
            }
        } else if (S_ISREG(status.st_mode) &&
                   upload_file(files, progress_entry, fd, child, directory, entry->d_name) != 0) {
            closedir(input);
            return -1;
        }
    }
    return closedir(input) == 0 ? 0 : -1;
}

static int upload_from_pen(struct files *files, struct file_entry *entry) {
    char path[FILE_PATH + FILE_NAME + 32];
    char directory[FILE_PATH];
    int fd;
    int result;
    if (!entry || snprintf(path, sizeof(path), "%s", files->pen.path) < 0 ||
        strlen(files->pen.path) >= sizeof(path) ||
        files_path_child(path, sizeof(path), entry->name) != 0)
        return -1;
    if (entry->directory &&
        upload_directories(files, path, files->windows.path, entry->name, directory,
                           sizeof(directory)) != 0)
        return -1;
    fd = files_remote_open(files, FILE_UPLOAD);
    if (fd < 0) return -1;
    result = entry->directory ? upload_tree(files, entry, fd, path, directory)
                              : upload_file(files, entry, fd, path, files->windows.path,
                                            entry->name);
    close(fd);
    return result;
}

int files_transfer(struct files *files, bool upload) {
    struct file_panel *source = upload ? &files->pen : &files->windows;
    int result = 0;
    size_t completed = 0;
    for (size_t index = 0; index != source->count; ++index) {
        struct file_entry *entry = &source->entries[index];
        if (entry->state == FILE_STATUS_SUCCESS || entry->state == FILE_STATUS_FAILED)
            entry->state = FILE_STATUS_NONE;
        if (entry->state != FILE_STATUS_SELECTED) continue;
        entry->total = 0;
        entry->done = 0;
        entry->progress = 0;
        entry->state = FILE_STATUS_TRANSFER;
    }
    files_state_write(files);
    for (size_t index = 0; index != source->count; ++index) {
        struct file_entry *entry = &source->entries[index];
        if (entry->state != FILE_STATUS_TRANSFER) continue;
        if (entry_total(files, upload, entry, &entry->total) != 0) {
            entry->state = FILE_STATUS_FAILED;
            result = -1;
        }
    }
    files_state_write(files);
    for (size_t index = 0; index != source->count; ++index) {
        struct file_entry *entry = &source->entries[index];
        if (entry->state != FILE_STATUS_TRANSFER) continue;
        if ((upload ? upload_from_pen(files, entry) : download_to_pen(files, entry)) != 0) {
            entry->state = FILE_STATUS_FAILED;
            result = -1;
        } else {
            entry->state = FILE_STATUS_SUCCESS;
            entry->progress = 100;
            ++completed;
        }
        files_state_write(files);
    }
    if (completed && files_refresh_panel(files, !upload) != 0) result = -1;
    files_state_write(files);
    return result;
}
