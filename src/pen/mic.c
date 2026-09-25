#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mic.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static int mixer(bool enabled) {
    pid_t process = fork();
    if (process < 0) return -1;
    if (!process) {
        execl("/bin/amixer", "amixer", "-c", "1", "cset", "numid=2", enabled ? "1" : "0",
              (char *) NULL);
        _exit(127);
    }
    int status;
    while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int recording_file(char *path, size_t size) {
    struct timespec now;
    struct tm local;
    char timestamp[16];
    time_t seconds;
    int milliseconds;
    if ((mkdir("/userdisk/PenDesk", 0700) != 0 && errno != EEXIST) ||
        (mkdir("/userdisk/PenDesk/Audio", 0700) != 0 && errno != EEXIST) ||
        clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    seconds = now.tv_sec;
    milliseconds = now.tv_nsec / 1000000;
    for (;;) {
        if (!localtime_r(&seconds, &local) ||
            !strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local) ||
            snprintf(path, size, "/userdisk/PenDesk/Audio/%s_%03d.flac", timestamp,
                     milliseconds) >= (int) size)
            return -1;
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (errno != EEXIST) return -1;
        if (++milliseconds == 1000) {
            milliseconds = 0;
            ++seconds;
        }
    }
}

void mic_init(struct microphone *microphone, const uint8_t nonce[8]) {
    *microphone = (struct microphone) {.fd = -1};
    memcpy(microphone->nonce, nonce, sizeof(microphone->nonce));
}

int mic_start(struct microphone *microphone) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    socklen_t address_length = sizeof(address);
    char port[8];
    char port_arg[16];
    char file_arg[144];
    pid_t process;
    int fd;
    if (microphone->process > 0 || mixer(true) != 0) return -1;
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        mixer(false);
        return -1;
    }
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        getsockname(fd, (struct sockaddr *) &address, &address_length) != 0) {
        close(fd);
        mixer(false);
        return -1;
    }
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    snprintf(port_arg, sizeof(port_arg), "port=%s", port);
    if (recording_file(microphone->path, sizeof(microphone->path)) != 0) {
        close(fd);
        mixer(false);
        return -1;
    }
    snprintf(file_arg, sizeof(file_arg), "location=%s", microphone->path);
    process = fork();
    if (process < 0) {
        unlink(microphone->path);
        microphone->path[0] = '\0';
        close(fd);
        mixer(false);
        return -1;
    }
    if (!process) {
        execl("/usr/bin/gst-launch-1.0", "gst-launch-1.0", "-e", "-q", "alsasrc", "device=mic_and_ref",
              "do-timestamp=true", "buffer-time=20000", "latency-time=10000", "!",
              "audio/x-raw,format=S32LE,rate=96000,channels=2", "!", "audioconvert",
              "mix-matrix=< <(float)1.0, (float)0.0> >", "!",
              "audio/x-raw,format=S32LE,rate=96000,channels=1", "!", "audioresample", "quality=10", "!",
              "audio/x-raw,format=S32LE,rate=16000,channels=1", "!", "tee", "name=audio",
              "audio.", "!", "queue", "!", "audioconvert", "!",
              "audio/x-raw,format=S24LE,rate=16000,channels=1", "!", "flacenc", "!", "filesink",
              file_arg, "audio.", "!", "queue", "!",
              "audioconvert", "!", "audio/x-raw,format=S16LE,rate=16000,channels=1", "!",
              "opusenc", "bitrate=256000", "frame-size=20", "audio-type=generic", "complexity=10",
              "bandwidth=wideband", "!", "rtpopuspay", "pt=111", "mtu=1200", "!",
              "udpsink", "host=127.0.0.1", port_arg, "sync=false", "async=false", (char *) NULL);
        _exit(127);
    }
    microphone->fd = fd;
    microphone->process = process;
    return 0;
}

int mic_running(struct microphone *microphone) {
    if (microphone->process <= 0 || waitpid(microphone->process, NULL, WNOHANG) != microphone->process)
        return microphone->process > 0;
    close(microphone->fd);
    microphone->fd = -1;
    microphone->process = 0;
    unlink(microphone->path);
    microphone->path[0] = '\0';
    mixer(false);
    return 0;
}

void mic_stop(struct microphone *microphone) {
    if (microphone->process > 0) {
        kill(microphone->process, SIGINT);
        while (waitpid(microphone->process, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (microphone->fd >= 0) close(microphone->fd);
    microphone->fd = -1;
    microphone->process = 0;
    mixer(false);
}

static int mic_send(const struct video *video, const struct cfg *cfg, bool recording,
                    const uint8_t *payload, size_t length) {
    uint8_t packet[10 + 14 + 1200];
    struct in_addr host;
    uint16_t port = htons(cfg->port);
    if (length > 1200 || inet_pton(AF_INET, cfg->host, &host) != 1) return -1;
    packet[0] = packet[1] = packet[2] = 0;
    packet[3] = 1;
    memcpy(packet + 4, &host, sizeof(host));
    memcpy(packet + 8, &port, sizeof(port));
    memcpy(packet + 10, "PDSM", 4);
    packet[14] = PROTOCOL_VERSION;
    memcpy(packet + 15, video->nonce, sizeof(video->nonce));
    packet[23] = recording ? 1 : 0;
    if (length) memcpy(packet + 24, payload, length);
    ssize_t sent = send(video->fd, packet, length + 24, 0);
    return sent == (ssize_t)(length + 24) ? 0 : -1;
}

int mic_state(const struct video *video, const struct cfg *cfg, bool recording) {
    return mic_send(video, cfg, recording, NULL, 0);
}

int mic_forward(struct microphone *microphone, const struct video *video, const struct cfg *cfg) {
    uint8_t packet[1200];
    for (;;) {
        ssize_t length = recv(microphone->fd, packet, sizeof(packet), 0);
        if (length < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!length) continue;
        if (mic_send(video, cfg, true, packet, (size_t) length) != 0 &&
            errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
}
