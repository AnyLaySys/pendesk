#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (strlen(text) >= sizeof(value)) return -1;
    snprintf(value, sizeof(value), "%s", text);
    end = strrchr(value, '/');
    if (!end) return -1;
    *end++ = '\0';
    if (!side(value, &pen) || !*end) return -1;
    row = strtoul(end, &end, 10);
    return *end ? -1 : (open ? panel_open(files, pen, (size_t) row) : panel_select(
            files, pen, (size_t) row));
}

struct files *
files_new(const char *host, uint16_t port, const char *socks_host, uint16_t socks_port,
          const uint8_t token[32]) {
    struct files *files;
    if (!host || !socks_host || !token) return NULL;
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
    unlink(FILE_STATE);
    return files;
}

void files_free(struct files *files) {
    if (!files) return;
    pthread_mutex_destroy(&files->mutex);
    free(files);
}

static void run_action(struct files *files, const char *action) {
    bool transfer = !strcmp(action, "transfer/push") || !strcmp(action, "transfer/pull");
    pthread_mutex_lock(&files->mutex);
    if (!strcmp(action, "reset")) {
        files->pen.path[0] = '/';
        files->pen.path[1] = '\0';
        files->windows.path[0] = '\0';
        files_refresh_panel(files, true);
        files_refresh_panel(files, false);
    } else if (!strncmp(action, "open/", 5)) {
        action_row(files, action + 5, true);
    } else if (!strncmp(action, "select/", 7)) {
        action_row(files, action + 7, false);
    } else if (!strcmp(action, "transfer/push")) {
        files_transfer(files, true);
    } else if (!strcmp(action, "transfer/pull")) {
        files_transfer(files, false);
    }
    if (!transfer) files_state_write(files);
    pthread_mutex_unlock(&files->mutex);
}

int files_submit_sync(struct files *files, const char *action) {
    if (!files || !action || strlen(action) >= 64) return -1;
    run_action(files, action);
    return 0;
}