#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "record.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int recording_file(const char *subdir, const char *extension, char *path, size_t size) {
    struct timespec now;
    struct tm local;
    char timestamp[16];
    char directory[64];
    time_t seconds;
    int milliseconds;
    if (snprintf(directory, sizeof(directory), "/userdisk/PenDesk/%s", subdir) >=
        (int) sizeof(directory) || (mkdir("/userdisk/PenDesk", 0700) != 0 && errno != EEXIST) ||
        (mkdir(directory, 0700) != 0 && errno != EEXIST) ||
        clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    seconds = now.tv_sec;
    milliseconds = now.tv_nsec / 1000000;
    for (;;) {
        if (!localtime_r(&seconds, &local) ||
            !strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local) ||
            snprintf(path, size, "%s/%s_%03d.%s", directory, timestamp, milliseconds, extension) >=
            (int) size)
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