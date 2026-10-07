#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cam.h"
#include "record.h"
#include "io.h"
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
#include <sys/wait.h>
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
        if (!(caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_CAPTURE)) ||
            !(caps & V4L2_CAP_STREAMING)) {
            close(fd);
            continue;
        }
        capture_type = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                                            : V4L2_BUF_TYPE_VIDEO_CAPTURE;
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

void cam_init(struct camera *camera) {
    *camera = (struct camera) {.fd = -1};
}

int cam_start(struct camera *camera) {
    char device[64];
    char executable[512];
    int descriptors[2];
    pid_t process;
    pid_t parent = getpid();
    if (camera->process > 0 || find_camera(device, sizeof(device)) != 0) return -1;
    if (recording_file("Video", "avi", camera->path, sizeof(camera->path)) != 0)
        return -1;
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size < 0) return -1;
    executable[size] = '\0';
    char *name = strrchr(executable, '/');
    if (!name || snprintf(name + 1, sizeof(executable) - (size_t)(name + 1 - executable), "pdc") != 3) return -1;
    if (pipe2(descriptors, O_CLOEXEC) != 0) return -1;
    process = fork();
    if (process < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        unlink(camera->path);
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
        execl(executable, "pdc", device, camera->path, (char *) NULL);
        _exit(127);
    }
    close(descriptors[1]);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    camera->fd = descriptors[0];
    camera->process = process;
    return 0;
}

static void release(struct camera *camera) {
    uint32_t sequence = camera->sequence;
    if (camera->fd >= 0) close(camera->fd);
    free(camera->incoming.data);
    free(camera->pending.data);
    free(camera->sending.data);
    cam_init(camera);
    camera->sequence = sequence;
}

void cam_stop(struct camera *camera) {
    io_stop_process(camera->process);
    release(camera);
}

int cam_running(struct camera *camera) {
    pid_t result;
    if (camera->process <= 0) return 0;
    do result = waitpid(camera->process, NULL, WNOHANG); while (result < 0 && errno == EINTR);
    if (result == 0) return 1;
    release(camera);
    return 0;
}
