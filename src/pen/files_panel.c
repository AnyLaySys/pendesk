#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "files_internal.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

bool files_valid_name(const char *name) {
    return *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/') &&
           !strchr(name, '\\');
}

static int entry_compare(const void *left, const void *right) {
    const struct file_entry *a = left;
    const struct file_entry *b = right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void panel_clear(struct file_panel *panel) {
    panel->count = 0;
}

int files_path_child(char *path, size_t size, const char *name) {
    size_t length = strlen(path);
    int result;
    if (!files_valid_name(name)) return -1;
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
    if (!strcmp(path, "/") || !*path) return;
    if (separator) {
        if (separator == path) path[1] = '\0';
        else *separator = '\0';
    } else path[0] = '\0';
}

static bool panel_parent(const struct file_panel *panel) {
    return panel->path[0] && strcmp(panel->path, "/");
}

static int local_list(struct file_panel *panel) {
    DIR *directory;
    struct dirent *entry;
    if (!(directory = opendir(panel->path))) return -1;
    panel_clear(panel);
    while ((entry = readdir(directory))) {
        struct stat status;
        char full[FILE_PATH + FILE_NAME + 32];
        struct file_entry *output;
        int length;
        if (!files_valid_name(entry->d_name) || panel->count == FILE_ENTRIES) continue;
        length = snprintf(full, sizeof(full), "%s%s%s", panel->path,
                          !strcmp(panel->path, "/") ? "" : "/", entry->d_name);
        if (length < 0 || (size_t) length >= sizeof(full) || lstat(full, &status) != 0 ||
            (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)))
            continue;
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

int files_remote_list(struct files *files, struct file_panel *panel) {
    uint16_t count;
    int fd = files_remote_open(files, FILE_LIST);
    if (fd < 0 || files_write_text(fd, panel->path) != 0 || files_remote_status(fd) != 0 ||
        files_read_u16(fd, &count) != 0) {
        if (fd >= 0) close(fd);
        panel_clear(panel);
        return -1;
    }
    panel_clear(panel);
    for (uint16_t index = 0; index != count; ++index) {
        uint8_t kind;
        struct file_entry item;
        if (files_read_all(fd, &kind, 1) != 0 ||
            files_read_text(fd, item.name, sizeof(item.name)) != 0 ||
            files_read_u64(fd, &item.size) != 0 || (kind != 0 && kind != 1) ||
            !files_valid_name(item.name)) {
            close(fd);
            return -1;
        }
        item.directory = kind != 0;
        item.state = FILE_STATUS_NONE;
        if (panel->count != FILE_ENTRIES) panel->entries[panel->count++] = item;
    }
    close(fd);
    return 0;
}

int files_refresh_panel(struct files *files, bool pen) {
    return pen ? local_list(&files->pen) : files_remote_list(files, &files->windows);
}

int files_panel_select(struct files *files, bool pen, size_t row) {
    struct file_panel *panel = pen ? &files->pen : &files->windows;
    size_t index;
    bool parent = panel_parent(panel);
    if (parent && !row) return -1;
    index = row - parent;
    if (index >= panel->count) return -1;
    panel->entries[index].state =
            panel->entries[index].state == FILE_STATUS_SELECTED ? FILE_STATUS_NONE
                                                                : FILE_STATUS_SELECTED;
    return 0;
}

int files_panel_open(struct files *files, bool pen, size_t row) {
    struct file_panel *panel = pen ? &files->pen : &files->windows;
    size_t index;
    bool parent = panel_parent(panel);
    if (parent && !row) {
        path_parent(panel->path);
        return files_refresh_panel(files, pen);
    }
    index = row - parent;
    if (index >= panel->count) return -1;
    return panel->entries[index].directory &&
           files_path_child(panel->path, sizeof(panel->path), panel->entries[index].name) == 0 &&
           files_refresh_panel(files, pen) == 0 ? 0 : -1;
}

static int state_text(FILE *output, const char *text) {
    const uint8_t *input = (const uint8_t *) text;
    if (fputc('"', output) == EOF) return -1;
    while (*input) {
        uint32_t value = *input++;
        if (value >= 0x80) {
            if (value >= 0xc2 && value <= 0xdf && input[0] && (input[0] & 0xc0) == 0x80)
                value = (value & 0x1f) << 6 | (*input++ & 0x3f);
            else if (value >= 0xe0 && value <= 0xef && input[0] && input[1] &&
                     (input[0] & 0xc0) == 0x80 && (input[1] & 0xc0) == 0x80) {
                value = (value & 0x0f) << 12 | (input[0] & 0x3f) << 6 | (input[1] & 0x3f);
                input += 2;
            } else if (value >= 0xf0 && value <= 0xf4 && input[0] && input[1] && input[2] &&
                       (input[0] & 0xc0) == 0x80 && (input[1] & 0xc0) == 0x80 &&
                       (input[2] & 0xc0) == 0x80) {
                value = (value & 0x07) << 18 | (input[0] & 0x3f) << 12 | (input[1] & 0x3f) << 6 |
                        (input[2] & 0x3f);
                input += 3;
            } else value = '?';
        }
        if (value >= 0x20 && value <= 0x7e && value != '"' && value != '\\') {
            if (fputc((int) value, output) == EOF) return -1;
        } else if (value <= 0xffff) {
            if (fprintf(output, "\\u%04x", value) < 0) return -1;
        } else if (fprintf(output, "\\u%04x\\u%04x", 0xd800 + ((value - 0x10000) >> 10),
                           0xdc00 + ((value - 0x10000) & 0x3ff)) < 0)
            return -1;
    }
    return fputc('"', output) == EOF ? -1 : 0;
}

static int state_entry(FILE *output, const struct file_entry *entry, bool parent) {
    if (fputs("{\"name\":", output) == EOF ||
        state_text(output, parent ? ".." : entry->name) != 0 ||
        fprintf(output, ",\"size\":%llu,\"directory\":%u,\"state\":%u,\"parent\":%u}",
                (unsigned long long) (parent ? 0 : entry->size), parent || entry->directory,
                parent ? FILE_STATUS_NONE : entry->state, parent) < 0)
        return -1;
    return 0;
}

static int state_panel(FILE *output, const struct file_panel *panel) {
    bool comma = false;
    if (fputc('[', output) == EOF) return -1;
    if (panel_parent(panel)) {
        if (state_entry(output, NULL, true) != 0) return -1;
        comma = true;
    }
    for (size_t index = 0; index != panel->count; ++index) {
        if (comma && fputc(',', output) == EOF) return -1;
        if (state_entry(output, &panel->entries[index], false) != 0) return -1;
        comma = true;
    }
    return fputc(']', output) == EOF ? -1 : 0;
}

int files_state_write(const struct files *files) {
    FILE *output = fopen(FILE_STATE_TEMP, "w");
    int result = -1;
    if (!output) return -1;
    if (fputs("{\"pen\":", output) != EOF && state_panel(output, &files->pen) == 0 &&
        fputs(",\"windows\":", output) != EOF && state_panel(output, &files->windows) == 0 &&
        fprintf(output, ",\"transfer\":%u,\"progress\":%u}", (unsigned int) files->transfer,
                (unsigned int) files->progress) >= 0)
        result = 0;
    if (fclose(output) != 0) result = -1;
    if (result != 0) {
        unlink(FILE_STATE_TEMP);
        return -1;
    }
    return rename(FILE_STATE_TEMP, FILE_STATE);
}