#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "files_internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *files_worker(void *argument);

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
    return *end || row > SIZE_MAX ? -1 : (open ? files_panel_open(files, pen, (size_t) row)
                                               : files_panel_select(files, pen, (size_t) row));
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
    if (pthread_mutex_init(&files->queue_mutex, NULL) != 0 ||
        pthread_cond_init(&files->queue_cond, NULL) != 0 ||
        pthread_create(&files->worker, NULL, files_worker, files) != 0) {
        pthread_mutex_destroy(&files->mutex);
        free(files);
        return NULL;
    }
    files->worker_started = true;
    return files;
}

void files_free(struct files *files) {
    if (!files) return;
    if (files->worker_started) {
        pthread_mutex_lock(&files->queue_mutex);
        files->stopping = true;
        pthread_cond_signal(&files->queue_cond);
        pthread_mutex_unlock(&files->queue_mutex);
        pthread_join(files->worker, NULL);
        pthread_cond_destroy(&files->queue_cond);
        pthread_mutex_destroy(&files->queue_mutex);
    }
    pthread_mutex_destroy(&files->mutex);
    free(files);
}

static void run_action(struct files *files, const char *action) {
    pthread_mutex_lock(&files->mutex);
    if (!strcmp(action, "reset")) {
        files->pen.path[0] = '/';
        files->pen.path[1] = '\0';
        files->windows.path[0] = '\0';
        files_refresh_panel(files, true);
        files_state_write(files);
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
    files_state_write(files);
    pthread_mutex_unlock(&files->mutex);
}

static void *files_worker(void *argument) {
    struct files *files = argument;
    pthread_mutex_lock(&files->queue_mutex);
    while (alive) {
        while (alive && !files->stopping && files->head == files->tail)
            pthread_cond_wait(&files->queue_cond, &files->queue_mutex);
        if (files->stopping || !alive) break;
        char action[QUEUE_ACTION];
        memcpy(action, files->queue[files->head % QUEUE_SLOTS], QUEUE_ACTION);
        ++files->head;
        pthread_mutex_unlock(&files->queue_mutex);
        run_action(files, action);
        pthread_mutex_lock(&files->queue_mutex);
    }
    pthread_mutex_unlock(&files->queue_mutex);
    return NULL;
}

int files_submit(struct files *files, const char *action) {
    size_t length;
    if (!files || !action || (length = strlen(action)) >= QUEUE_ACTION) return -1;
    pthread_mutex_lock(&files->queue_mutex);
    if (files->tail - files->head < QUEUE_SLOTS) {
        memcpy(files->queue[files->tail % QUEUE_SLOTS], action, length + 1);
        ++files->tail;
        pthread_cond_signal(&files->queue_cond);
    }
    pthread_mutex_unlock(&files->queue_mutex);
    return 0;
}