#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "bytes.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

void audio_init(struct audio *audio) {
    audio->socket = -1;
    audio->process = 0;
}

int audio_start(struct audio *audio) {
    char port_arg[12];
    int reservation;
    int socket_fd;
    socklen_t address_length = sizeof(audio->address);
    pid_t process;
    pid_t parent = getpid();
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    char *arguments[] = {"gst-launch-1.0", "-q", "udpsrc", "address=127.0.0.1", port_arg,
                         "caps=application/x-rtp,media=(string)audio,encoding-name=(string)OPUS,clock-rate=(int)48000,encoding-params=(string)2,payload=(int)111",
                         "!", "rtpjitterbuffer", "latency=20", "drop-on-latency=true", "!",
                         "rtpopusdepay", "!", "opusdec", "!", "audioconvert", "!", "audioresample",
                         "!", "audio/x-raw,format=S16LE,rate=48000,channels=2", "!", "alsasink",
                         "device=default", NULL};
    if (audio->process > 0) {
        pid_t result = waitpid(audio->process, NULL, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return -1;
        close(audio->socket);
        audio->socket = -1;
        audio->process = 0;
    }
    reservation = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (reservation < 0) return -1;
    if (bind(reservation, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        getsockname(reservation, (struct sockaddr *) &address, &address_length) != 0) {
        close(reservation);
        return -1;
    }
    close(reservation);
    snprintf(port_arg, sizeof(port_arg), "port=%u", ntohs(address.sin_port));
    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return -1;
    process = fork();
    if (process < 0) {
        close(socket_fd);
        return -1;
    }
    if (!process) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(127);
        int null = open("/dev/null", O_WRONLY);
        if (null < 0 || dup2(null, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0)
            _exit(127);
        if (null > STDERR_FILENO) close(null);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    audio->socket = socket_fd;
    audio->process = process;
    audio->address = address;
    return 0;
}

void audio_play(struct audio *audio, uint32_t sequence, const uint8_t *frame, size_t length) {
    uint8_t packet[12 + 1152];
    uint32_t timestamp = sequence * 960U;
    if (audio->process <= 0 || !length || length > 1152) return;
    if (waitpid(audio->process, NULL, WNOHANG) == audio->process) {
        close(audio->socket);
        audio->socket = -1;
        audio->process = 0;
        return;
    }
    packet[0] = 0x80;
    packet[1] = 111;
    write_u16(packet + 2, (uint16_t) sequence);
    write_u32(packet + 4, timestamp);
    memcpy(packet + 8, "PDSK", 4);
    memcpy(packet + 12, frame, length);
    sendto(audio->socket, packet, 12 + length, MSG_DONTWAIT, (struct sockaddr *) &audio->address,
           sizeof(audio->address));
}

void audio_stop(struct audio *audio) {
    if (audio->socket >= 0) close(audio->socket);
    audio->socket = -1;
    if (audio->process <= 0) return;
    kill(audio->process, SIGKILL);
    while (waitpid(audio->process, NULL, 0) < 0 && errno == EINTR) {}
    audio->process = 0;
}