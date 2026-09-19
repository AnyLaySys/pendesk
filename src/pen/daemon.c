#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cfg.h"
#include "input.h"
#include "link.h"
#include "preview.h"
#include "video.h"
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t alive = 1;

static void stop(int signal) {
    (void) signal;
    alive = 0;
}

static void close_inherited(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return;
    int current = dirfd(directory);
    for (struct dirent *entry; (entry = readdir(directory));) {
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != current) close(fd);
    }
    closedir(directory);
}

int main(int argc, char **argv) {
    struct cfg cfg = {0};
    struct input_state input;
    struct preview preview;
    struct link link;
    struct video video = {.association = -1, .fd = -1};
    close_inherited();
    if (argc == 4 && !strcmp(argv[1], "--input")) return input_send_control(argv[2], argv[3]);
    if (argc != 3 || (strcmp(argv[1], "--config") && strcmp(argv[1], "--check-config")) ||
        cfg_load(argv[2], &cfg) != 0)
        return 1;
    if (!strcmp(argv[1], "--check-config")) return 0;
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (input_open(&input, argv[2]) != 0) return 1;
    if (preview_open(&preview, &cfg) != 0) {
        input_close(&input);
        return 1;
    }
    link_init(&link);
    while (alive) {
        struct mode mode;
        if (input_display_mode(&mode) == 0 && link_open(&link, &cfg) == 0) {
            if (link_video_open(&video, &cfg) == 0 &&
                link_handshake(&link, &cfg, mode, video.nonce) == 0) {
                link_start(&link);
                if (input_start(&input, &link, mode) == 0) {
                    uint8_t state[] = {0x25, input_paused(&input) ? 0 : 1};
                    if (link_send(&link, state, sizeof(state)) == 0)
                        video_receive(&input, &preview, &video, &cfg);
                    input_stop(&input);
                } else link_stop(&link);
            }
            link_video_close(&video);
            link_close(&link);
        }
        if (alive) sleep(1);
    }
    preview_close(&preview);
    input_close(&input);
    link_destroy(&link);
    return 0;
}