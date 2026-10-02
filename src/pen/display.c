#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "display.h"
#include "input.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int display_mode(struct mode *mode) {
    DIR *directory = opendir("/sys/class/drm");
    struct dirent *entry;
    if (!directory) return -1;
    while ((entry = readdir(directory))) {
        char status[256];
        char modes[256];
        char value[64];
        FILE *file;
        unsigned int width;
        unsigned int height;
        if (strncmp(entry->d_name, "card", 4) || !strchr(entry->d_name, '-') ||
            strstr(entry->d_name, "Writeback") ||
            snprintf(status, sizeof(status), "/sys/class/drm/%s/status", entry->d_name) >=
            (int) sizeof(status) ||
            snprintf(modes, sizeof(modes), "/sys/class/drm/%s/modes", entry->d_name) >=
            (int) sizeof(modes))
            continue;
        file = fopen(status, "r");
        if (!file) continue;
        if (!fgets(value, sizeof(value), file) || strcmp(value, "connected\n")) {
            fclose(file);
            continue;
        }
        fclose(file);
        file = fopen(modes, "r");
        if (!file) continue;
        if (fscanf(file, "%ux%u", &width, &height) == 2 && width && height && width <= UINT16_MAX &&
            height <= UINT16_MAX) {
            fclose(file);
            closedir(directory);
            mode->width = (uint16_t) width;
            mode->height = (uint16_t) height;
            return 0;
        }
        fclose(file);
    }
    closedir(directory);
    return -1;
}
