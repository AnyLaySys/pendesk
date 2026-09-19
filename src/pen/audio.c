#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void audio_init(struct audio *audio) {
    audio->input = -1;
    audio->process = 0;
}

int audio_start(struct audio *audio, uint32_t rate, uint8_t channels) {
    char rate_text[16];
    char channels_text[8];
    int input[2];
    posix_spawn_file_actions_t actions;
    pid_t process;
    if (audio->process > 0 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000)
        return -1;
    snprintf(rate_text, sizeof(rate_text), "%u", rate);
    snprintf(channels_text, sizeof(channels_text), "%u", channels);
    char *arguments[] = {"aplay", "-D", "default", "-f", "S16_LE", "-r", rate_text, "-c",
                         channels_text, "-q", "-", NULL};
    if (pipe(input) != 0) return -1;
    fcntl(input[0], F_SETFD, FD_CLOEXEC);
    fcntl(input[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (input[0] > 2) posix_spawn_file_actions_addclose(&actions, input[0]);
    if (input[1] > 2) posix_spawn_file_actions_addclose(&actions, input[1]);
    int result = posix_spawnp(&process, arguments[0], &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    if (result != 0) {
        close(input[1]);
        return -1;
    }
    fcntl(input[1], F_SETPIPE_SZ, 256 * 1024);
    fcntl(input[1], F_SETFL, O_NONBLOCK);
    audio->input = input[1];
    audio->process = process;
    return 0;
}

void audio_play(struct audio *audio, const uint8_t *samples, size_t length) {
    if (audio->process <= 0) return;
    while (length) {
        ssize_t written = write(audio->input, samples, length);
        if (written > 0) {
            samples += written;
            length -= (size_t) written;
            continue;
        }
        return;
    }
}

void audio_stop(struct audio *audio) {
    if (audio->process <= 0) return;
    close(audio->input);
    kill(audio->process, SIGTERM);
    while (waitpid(audio->process, NULL, 0) < 0 && errno == EINTR) {}
    audio->input = -1;
    audio->process = 0;
}
