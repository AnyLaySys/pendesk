#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cam.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int ioctl_retry(int fd, unsigned long request, void *argument) {
    int result;
    do result = ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
    return result;
}

static int node_size(int fd, enum v4l2_buf_type type, uint32_t *width, uint32_t *height) {
    bool found = false;
    for (uint32_t index = 0;; ++index) {
        struct v4l2_frmsizeenum size = {.index = index, .pixel_format = V4L2_PIX_FMT_NV12};
        if (ioctl_retry(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) break;
        uint32_t w = size.type == V4L2_FRMSIZE_TYPE_DISCRETE ? size.discrete.width
                                                             : size.stepwise.max_width;
        uint32_t h = size.type == V4L2_FRMSIZE_TYPE_DISCRETE ? size.discrete.height
                                                             : size.stepwise.max_height;
        if (w && h && (!found || (uint64_t) w * h > (uint64_t) * width * *height)) {
            *width = w;
            *height = h;
            found = true;
        }
        if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
    }
    if (found) return 0;
    struct v4l2_format format = {.type = type};
    if (ioctl_retry(fd, VIDIOC_G_FMT, &format) != 0) return -1;
    *width = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? format.fmt.pix_mp.width
                                                        : format.fmt.pix.width;
    *height = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? format.fmt.pix_mp.height
                                                         : format.fmt.pix.height;
    return *width && *height ? 0 : -1;
}

static int find_camera(char *path, size_t path_size) {
    DIR *directory = opendir("/dev");
    uint64_t best_pixels = 0;
    int best_index = INT32_MAX;
    if (!directory) return -1;
    for (struct dirent *entry; (entry = readdir(directory));) {
        char *end;
        long index;
        char device[64];
        int fd;
        struct v4l2_capability capability = {0};
        uint32_t capture_type;
        uint32_t node_width = 0;
        uint32_t node_height = 0;
        uint64_t pixels;
        if (strncmp(entry->d_name, "video", 5)) continue;
        index = strtol(entry->d_name + 5, &end, 10);
        if (*end || index < 0 || index >= INT32_MAX) continue;
        snprintf(device, sizeof(device), "/dev/video%ld", index);
        fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) != 0) {
            close(fd);
            continue;
        }
        uint32_t caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS ? capability.device_caps
                                                                       : capability.capabilities;
        if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        else if (caps & V4L2_CAP_VIDEO_CAPTURE)
            capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        else {
            close(fd);
            continue;
        }
        if (!(caps & V4L2_CAP_STREAMING)) {
            close(fd);
            continue;
        }
        bool nv12 = false;
        for (uint32_t format_index = 0;; ++format_index) {
            struct v4l2_fmtdesc format = {.index = format_index, .type = capture_type};
            if (ioctl_retry(fd, VIDIOC_ENUM_FMT, &format) != 0) break;
            if (format.pixelformat == V4L2_PIX_FMT_NV12) nv12 = true;
        }
        if (!nv12 || node_size(fd, capture_type, &node_width, &node_height) != 0) {
            close(fd);
            continue;
        }
        pixels = (uint64_t) node_width * node_height;
        if (pixels > best_pixels || (pixels == best_pixels && index < best_index)) {
            if (snprintf(path, path_size, "%s", device) >= (int) path_size) {
                close(fd);
                closedir(directory);
                return -1;
            }
            best_pixels = pixels;
            best_index = (int) index;
        }
        close(fd);
    }
    closedir(directory);
    return best_pixels ? 0 : -1;
}

static int recording_file(char *path, size_t size) {
    struct timespec now;
    struct tm local;
    char timestamp[16];
    time_t seconds;
    int milliseconds;
    if ((mkdir("/userdisk/PenDesk", 0700) != 0 && errno != EEXIST) ||
        (mkdir("/userdisk/PenDesk/Video", 0700) != 0 && errno != EEXIST) ||
        clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    seconds = now.tv_sec;
    milliseconds = now.tv_nsec / 1000000;
    for (;;) {
        if (!localtime_r(&seconds, &local) ||
            !strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local) ||
            snprintf(path, size, "/userdisk/PenDesk/Video/%s_%03d.avi", timestamp, milliseconds) >=
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

void cam_init(struct camera *camera) {
    *camera = (struct camera) {.fd = -1};
}

int cam_start(struct camera *camera, const struct cfg *cfg) {
    char device[64];
    char device_arg[80];
    char file_arg[192];
    char host[INET_ADDRSTRLEN];
    int descriptors[2];
    pid_t process;
    pid_t parent = getpid();
    if (camera->process > 0 || find_camera(device, sizeof(device)) != 0) return -1;
    if (inet_pton(AF_INET, cfg->host, host) != 1 ||
        recording_file(camera->path, sizeof(camera->path)) != 0)
        return -1;
    snprintf(device_arg, sizeof(device_arg), "device=%s", device);
    snprintf(file_arg, sizeof(file_arg), "location=%s", camera->path);
    if (pipe2(descriptors, O_CLOEXEC) != 0) return -1;
    process = fork();
    if (process < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        unlink(camera->path);
        camera->path[0] = '\0';
        return -1;
    }
    if (!process) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(127);
        int null = open("/dev/null", O_WRONLY);
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);
        if (null >= 0) dup2(null, STDERR_FILENO);
        if (descriptors[1] > STDERR_FILENO) close(descriptors[1]);
        if (null > STDERR_FILENO) close(null);
        execl("/usr/bin/gst-launch-1.0", "gst-launch-1.0", "-e", "-q", "v4l2src", device_arg,
              "io-mode=mmap", "!", "video/x-raw,format=NV12,width=1280,height=720", "!", "jpegenc",
              "quality=60", "!", "tee", "name=frames", "frames.", "!", "queue", "leaky=downstream",
              "max-size-buffers=1", "max-size-bytes=0", "max-size-time=0", "!", "fdsink", "fd=1",
              "sync=false", "frames.", "!", "queue", "leaky=downstream", "max-size-buffers=1",
              "max-size-bytes=0", "max-size-time=0", "!", "avimux", "!", "filesink", file_arg,
              (char *) NULL);
        _exit(127);
    }
    close(descriptors[1]);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    camera->fd = descriptors[0];
    camera->process = process;
    camera->sequence = 0;
    camera->capacity = CAMERA_MAX_FRAME;
    camera->frame = malloc(camera->capacity);
    if (!camera->frame) {
        cam_stop(camera);
        return -1;
    }
    return 0;
}

void cam_stop(struct camera *camera) {
    if (camera->process > 0) {
        kill(camera->process, SIGKILL);
        while (waitpid(camera->process, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (camera->fd >= 0) close(camera->fd);
    free(camera->frame);
    cam_init(camera);
}

int cam_running(struct camera *camera) {
    int status;
    pid_t result;
    if (camera->process <= 0) return 0;
    do result = waitpid(camera->process, &status, WNOHANG); while (result < 0 && errno == EINTR);
    if (result == 0) return 1;
    if (camera->fd >= 0) close(camera->fd);
    free(camera->frame);
    cam_init(camera);
    return 0;
}
