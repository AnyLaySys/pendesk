#define _GNU_SOURCE
#include "../common/stream.h"
#include "../common/protocol.h"
#include "../common/pan.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>
#include <gst/video/video.h>
#include <quickjs.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/un.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

extern void registerCModuleLoader(const char *, JSModuleDef *(*)(JSContext *, const char *));

static char *control_path;

static JSValue send_control(JSContext *context, JSValueConst self, int count, JSValueConst *args) {
    if (count != 1) return JS_ThrowTypeError(context, "expected an input buffer");
    size_t length;
    uint8_t *bytes = JS_GetArrayBuffer(context, &length, args[0]);
    if (!bytes) return JS_EXCEPTION;
    if (length > PIPE_BUF) return JS_ThrowRangeError(context, "input buffer is too large");
    int fd = open(control_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return JS_FALSE;
    ssize_t written;
    do { written = write(fd, bytes, length); } while (written < 0 && errno == EINTR);
    close(fd);
    return JS_NewBool(context, written == (ssize_t)length);
}

static int control_init(JSContext *context, JSModuleDef *module) {
    if (!control_path) {
        Dl_info info;
        if (!dladdr((void *)send_control, &info)) return -1;
        char *library = g_canonicalize_filename(info.dli_fname, NULL);
        char *directory = g_path_get_dirname(library);
        control_path = g_canonicalize_filename("../../data/input.fifo", directory);
        g_free(directory);
        g_free(library);
    }
    JSValue function = JS_NewCFunction(context, send_control, "control", 1);
    if (JS_IsException(function)) return -1;
    return JS_SetModuleExport(context, module, "default", function);
}

static JSModuleDef *control_load(JSContext *context, const char *name) {
    JSModuleDef *module = JS_NewCModule(context, name, control_init);
    if (module) JS_AddModuleExport(context, module, "default");
    return module;
}

struct source {
    GWeakRef element;
    GQueue buffers;
    GstBuffer *buffer;
    GstMapInfo map;
    struct stream_packet packet;
    size_t received;
    size_t bytes;
    uint64_t newest;
    int fd;
    gint64 retry;
    guint64 first;
    GstClockTime base;
    gboolean started;
    gboolean waiting;
    gint64 requested;
};

static GWeakRef pipeline;
static GWeakRef sink;
static GWeakRef flip;
static struct pan_shared *pan;
static gint64 sampled_at;
static guint64 sampled_frames;
static double sampled_fps;
static GMutex view_mutex;
static double view_x, view_y;
static guint view_width, view_height;
static gint64 view_updated, next_expose, last_video;
static GMutex stamp_mutex;
static struct { GstClockTime pts; GstMemory *memory; gint64 time; } stamps[16];
static guint stamp_index;
static guint64 checked_frames, shared_frames;
static gint64 decoder_to_sink_max;

static struct pan_shared *shared_view(void) {
    g_mutex_lock(&view_mutex);
    if (!pan) pan = pan_open(0);
    struct pan_shared *shared = pan;
    g_mutex_unlock(&view_mutex);
    return shared;
}

static void sample(void) {
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&stamp_mutex);
    gboolean due = now - sampled_at >= G_USEC_PER_SEC;
    g_mutex_unlock(&stamp_mutex);
    if (!due) return;
    GstElement *element = g_weak_ref_get(&sink);
    if (!element) return;
    GstStructure *stats = NULL;
    g_object_get(element, "stats", &stats, NULL);
    guint64 rendered = 0, dropped = 0;
    if (stats) {
        gst_structure_get_uint64(stats, "rendered", &rendered);
        gst_structure_get_uint64(stats, "dropped", &dropped);
        gst_structure_free(stats);
    }
    g_mutex_lock(&stamp_mutex);
    if (sampled_at && now > sampled_at && rendered >= sampled_frames)
        sampled_fps = (double)(rendered - sampled_frames) * G_USEC_PER_SEC / (now - sampled_at);
    sampled_at = now;
    sampled_frames = rendered;
    double fps = sampled_fps;
    guint64 shared = shared_frames, checked = checked_frames;
    gint64 elapsed = decoder_to_sink_max;
    decoder_to_sink_max = 0;
    g_mutex_unlock(&stamp_mutex);
    FILE *file = fopen("/tmp/pendesk-video.stats", "w");
    if (file) {
        fprintf(file, "fps=%.2f\nrendered=%llu\ndropped=%llu\n", fps,
                (unsigned long long)rendered, (unsigned long long)dropped);
        struct pan_shared *view = shared_view();
        if (view) {
            uint64_t region = atomic_load(&view->region);
            uint32_t origin = atomic_load(&view->rendered);
            fprintf(file, "canvas=%ux%u\nviewport=%u,%u\n", (unsigned int)(region >> 48),
                    (unsigned int)((region >> 32) & 65535), origin >> 16, origin & 65535);
        }
        fprintf(file, "shared_buffers=%llu/%llu\ndecoder_to_sink_max_us=%lld\n",
                (unsigned long long)shared, (unsigned long long)checked, (long long)elapsed);
        fclose(file);
    }
    gst_object_unref(element);
}

static gboolean update_display(GstElement *element, gboolean cached, GstBuffer *buffer) {
    struct pan_shared *pan = shared_view();
    gint display_width = 0, display_height = 0;
    g_object_get(element, "display-width", &display_width, "display-height", &display_height, NULL);
    if (display_width <= 0 || display_height <= 0) return TRUE;
    uint64_t region = pan ? atomic_load(&pan->region) : 0;
    uint32_t display = pan ? atomic_load(&pan->display) : 0;
    guint width = region >> 48, height = (region >> 32) & 65535;
    guint visible_width = display >> 16, visible_height = display & 65535;
    if (!visible_width || !visible_height || width < visible_width || height < visible_height) {
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(element), 0, 0, display_width, display_height);
        return TRUE;
    }
    GstVideoMeta *layout = gst_buffer_get_video_meta(buffer);
    if (layout && (layout->width < height || layout->height < width)) return FALSE;
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&view_mutex);
    if (cached && (now < next_expose || now - last_video < 18000)) { g_mutex_unlock(&view_mutex); return FALSE; }
    if (!cached) last_video = now;
    double x = ((region >> 16) & 65535) / 16.0, y = (region & 65535) / 16.0;
    if (view_width != width || view_height != height) {
        view_x = x;
        view_y = y;
        view_width = width;
        view_height = height;
    } else {
        double elapsed = now - view_updated;
        double amount = elapsed / (elapsed + 2000.0);
        view_x += (x - view_x) * amount;
        view_y += (y - view_y) * amount;
    }
    view_updated = now;
    gint origin_x = CLAMP((gint)(view_x + 0.5), 0, (gint)(width - visible_width));
    gint origin_y = CLAMP((gint)(view_y + 0.5), 0, (gint)(height - visible_height));
    uint32_t position = (uint32_t)origin_x << 16 | (uint32_t)origin_y;
    gboolean changed = position != atomic_load(&pan->rendered);
    if (cached && !changed) { g_mutex_unlock(&view_mutex); return FALSE; }
    if (cached) {
        next_expose += 16667;
        if (next_expose <= now) next_expose = now + 16667;
    }
    g_mutex_unlock(&view_mutex);
    GstVideoOrientationMethod direction = GST_VIDEO_ORIENTATION_IDENTITY;
    GstElement *orientation = g_weak_ref_get(&flip);
    if (orientation) {
        g_object_get(orientation, "video-direction", &direction, NULL);
        gst_object_unref(orientation);
    }
    gint left = origin_y, top = width - visible_width - origin_x;
    if (direction == GST_VIDEO_ORIENTATION_180) {
        left = height - visible_height - origin_y;
        top = origin_x;
    }
    GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta(buffer);
    if (!crop) crop = gst_buffer_add_video_crop_meta(buffer);
    if (crop) {
        crop->x = (guint)left;
        crop->y = (guint)top;
        crop->width = visible_height;
        crop->height = visible_width;
        if (layout && width == visible_width && height == visible_height) {
            crop->x = crop->y = 0;
            crop->width = layout->width;
            crop->height = layout->height;
        }
    }
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(element), 0, 0, display_width, display_height);
    atomic_store(&pan->rendered, position);
    atomic_store(&pan->rotated, direction == GST_VIDEO_ORIENTATION_180);
    return TRUE;
}

static void repaint(GstElement *element) {
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&view_mutex);
    gboolean due = now >= next_expose && now - last_video >= 18000;
    g_mutex_unlock(&view_mutex);
    if (!due) return;
    struct pan_shared *view = shared_view();
    if (!view) return;
    uint64_t region = atomic_load(&view->region);
    uint32_t target = (uint32_t)((((region >> 16) & 65535) + 8) / 16) << 16 |
                      (uint32_t)(((region & 65535) + 8) / 16);
    if (target == atomic_load(&view->rendered)) return;
    GstSample *sample = NULL;
    g_object_get(element, "last-sample", &sample, NULL);
    if (!sample) return;
    GstBuffer *buffer = gst_buffer_copy(gst_sample_get_buffer(sample));
    gst_sample_unref(sample);
    if (!buffer) return;
    GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
    gst_mini_object_set_qdata(GST_MINI_OBJECT(buffer), g_quark_from_static_string("pendesk-cached"), GINT_TO_POINTER(1), NULL);
    GstPad *pad = gst_element_get_static_pad(element, "sink");
    gst_pad_chain(pad, buffer);
    gst_object_unref(pad);
}

static void clear_source(struct source *source) {
    if (source->buffer) {
        gst_buffer_unmap(source->buffer, &source->map);
        gst_buffer_unref(source->buffer);
        source->buffer = NULL;
    }
    g_queue_clear_full(&source->buffers, (GDestroyNotify)gst_buffer_unref);
    source->received = source->bytes = 0;
    source->newest = 0;
}

static int receive_source(struct source *source) {
    for (;;) {
        gboolean header = source->received < sizeof(source->packet);
        uint8_t *target = header ? (uint8_t *)&source->packet : source->map.data;
        size_t offset = header ? source->received : source->received - sizeof(source->packet);
        size_t length = header ? sizeof(source->packet) : source->packet.length;
        ssize_t count = recv(source->fd, target + offset, length - offset, MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!count) return -1;
        source->received += (size_t)count;
        if (header && source->received == sizeof(source->packet)) {
            if (!source->packet.length || source->packet.length > MAX_FRAME ||
                source->bytes + sizeof(source->packet) + source->packet.length > MAX_FRAME * 2)
                return -1;
            GstBuffer *buffer = gst_buffer_new_allocate(NULL, source->packet.length, NULL);
            if (!buffer) return -1;
            if (!gst_buffer_map(buffer, &source->map, GST_MAP_WRITE)) {
                gst_buffer_unref(buffer);
                return -1;
            }
            source->buffer = buffer;
            source->bytes += sizeof(source->packet) + source->packet.length;
            source->newest = source->packet.timestamp;
            GST_BUFFER_PTS(buffer) = source->packet.timestamp;
            if (!source->packet.keyframe) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        }
        if (!header && source->received == sizeof(source->packet) + source->packet.length) {
            gst_buffer_unmap(source->buffer, &source->map);
            g_queue_push_tail(&source->buffers, source->buffer);
            source->buffer = NULL;
            source->received = 0;
        }
    }
}

static void release_source(gpointer data) {
    struct source *source = data;
    if (source->fd >= 0) close(source->fd);
    g_weak_ref_clear(&source->element);
    clear_source(source);
    g_free(source);
}

static gboolean feed(gpointer data) {
    struct source *source = data;
    GstElement *element = g_weak_ref_get(&source->element);
    if (!element) return G_SOURCE_REMOVE;
    if (GST_STATE(element) < GST_STATE_PAUSED) {
        if (source->fd >= 0) close(source->fd);
        source->fd = -1;
        source->started = FALSE;
        clear_source(source);
        goto done;
    }
    if (source->fd < 0) {
        gint64 now = g_get_monotonic_time();
        if (now < source->retry) goto done;
        source->retry = now + 100000;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        memcpy(address.sun_path, STREAM_PATH, sizeof(STREAM_PATH));
        if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
            if (fd >= 0) close(fd);
            goto done;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        source->fd = fd;
    }
    guint64 queued = 0;
    g_object_get(element, "current-level-buffers", &queued, NULL);
    if (queued < 2) {
        if (receive_source(source) != 0) goto ended;
        GstBuffer *buffer;
        while ((buffer = g_queue_pop_head(&source->buffers))) {
            source->bytes -= sizeof(source->packet) + gst_buffer_get_size(buffer);
            uint64_t timestamp = GST_BUFFER_PTS(buffer);
            gboolean keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            GstClock *clock = gst_element_get_clock(element);
            GstClockTime now = clock ? gst_clock_get_time(clock) - gst_element_get_base_time(element) : 0;
            if (clock) gst_object_unref(clock);
            GstClockTime pts = source->base + (timestamp - source->first) * GST_MSECOND / 90;
            if (source->started && now > pts + 80 * GST_MSECOND) source->waiting = TRUE;
            gboolean old = source->newest > timestamp + 4500;
            if (old) source->waiting = TRUE;
            if (source->waiting && (!keyframe || old)) {
                gint64 wall = g_get_monotonic_time();
                if (wall - source->requested >= 100000) {
                    ssize_t sent = send(source->fd, "K", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
                    (void)sent;
                    source->requested = wall;
                }
                gst_buffer_unref(buffer);
                continue;
            }
            gboolean discontinuity = source->waiting;
            if (source->waiting) { source->started = FALSE; source->waiting = FALSE; }
            if (!source->started) {
                source->base = now;
                source->first = timestamp;
                source->started = TRUE;
            }
            GST_BUFFER_PTS(buffer) = source->base + (timestamp - source->first) * GST_MSECOND / 90;
            GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
            GST_BUFFER_DURATION(buffer) = GST_SECOND / 60;
            if (discontinuity) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            if (gst_app_src_push_buffer(GST_APP_SRC(element), buffer) != GST_FLOW_OK) goto ended;
            if (++queued >= 2) break;
        }
    }
    GstElement *display = g_weak_ref_get(&sink);
    if (display) { repaint(display); gst_object_unref(display); }
    sample();
    done:
    gst_object_unref(element);
    return G_SOURCE_CONTINUE;
    ended:
    gst_app_src_end_of_stream(GST_APP_SRC(element));
    gst_object_unref(element);
    return G_SOURCE_REMOVE;
}

static GstMemory *memory(GstBuffer *buffer) {
    if (!gst_buffer_n_memory(buffer)) return NULL;
    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    while (memory->parent) memory = memory->parent;
    return memory;
}

static GstPadProbeReturn decoded(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad;
    (void)data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    g_mutex_lock(&stamp_mutex);
    guint index = stamp_index++ % 16;
    stamps[index].pts = GST_BUFFER_PTS(buffer);
    stamps[index].memory = memory(buffer);
    stamps[index].time = g_get_monotonic_time();
    g_mutex_unlock(&stamp_mutex);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn fill_display(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad;
    GstElement *element = data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    g_mutex_lock(&stamp_mutex);
    for (guint i = 0; i < 16; ++i) {
        if (stamps[i].memory && stamps[i].pts == GST_BUFFER_PTS(buffer)) {
            ++checked_frames;
            if (stamps[i].memory == memory(buffer)) ++shared_frames;
            gint64 elapsed = g_get_monotonic_time() - stamps[i].time;
            if (elapsed > decoder_to_sink_max) decoder_to_sink_max = elapsed;
            break;
        }
    }
    g_mutex_unlock(&stamp_mutex);
    gboolean cached = gst_mini_object_get_qdata(GST_MINI_OBJECT(buffer), g_quark_from_static_string("pendesk-cached")) != NULL;
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    return update_display(element, cached, buffer) ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
}

static GstPadProbeReturn crop_view(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void)pad;
    (void)info;
    GstElement *element = data;
    gint left, right, top, bottom;
    g_object_get(element, "left", &left, "right", &right, "top", &top, "bottom", &bottom, NULL);
    if (left || right || top || bottom)
        g_object_set(element, "left", 0, "right", 0, "top", 0, "bottom", 0, NULL);
    return GST_PAD_PROBE_OK;
}

static void orient(GObject *object, GParamSpec *property, gpointer data) {
    (void)property;
    (void)data;
    GstVideoOrientationMethod direction;
    g_object_get(object, "video-direction", &direction, NULL);
    if (direction == GST_VIDEO_ORIENTATION_90L)
        g_object_set(object, "video-direction", GST_VIDEO_ORIENTATION_IDENTITY, NULL);
    else if (direction == GST_VIDEO_ORIENTATION_90R)
        g_object_set(object, "video-direction", GST_VIDEO_ORIENTATION_180, NULL);
}

static void tune(GstElement *element) {
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (!strcmp(name, "kmssink")) {
        g_object_set(element, "sync", TRUE, "qos", TRUE, "max-lateness", (gint64)(30 * GST_MSECOND),
                     "processing-deadline", (guint64)0, "skip-vsync", TRUE, "force-aspect-ratio", FALSE, NULL);
        if (!g_object_get_data(G_OBJECT(element), "pendesk-display")) {
            GstPad *pad = gst_element_get_static_pad(element, "sink");
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, fill_display, element, NULL);
            gst_object_unref(pad);
            g_object_set_data(G_OBJECT(element), "pendesk-display", GINT_TO_POINTER(1));
        }
        g_weak_ref_set(&sink, element);
        g_mutex_lock(&stamp_mutex);
        sampled_at = 0;
        sampled_frames = 0;
        sampled_fps = 0;
        memset(stamps, 0, sizeof(stamps));
        checked_frames = shared_frames = 0;
        decoder_to_sink_max = 0;
        g_mutex_unlock(&stamp_mutex);
    } else if (!strcmp(name, "mppvideodec")) {
        g_object_set(element, "dma-feature", TRUE, "fast-mode", TRUE, NULL);
        if (!g_object_get_data(G_OBJECT(element), "pendesk-decoder")) {
            GstPad *pad = gst_element_get_static_pad(element, "src");
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, decoded, NULL, NULL);
            gst_object_unref(pad);
            g_object_set_data(G_OBJECT(element), "pendesk-decoder", GINT_TO_POINTER(1));
        }
    } else if (!strcmp(name, "videoflip")) {
        g_weak_ref_set(&flip, element);
        if (!g_object_get_data(G_OBJECT(element), "pendesk-orientation")) {
            g_object_set_data(G_OBJECT(element), "pendesk-orientation", GINT_TO_POINTER(1));
            g_signal_connect(element, "notify::video-direction", G_CALLBACK(orient), NULL);
        }
        orient(G_OBJECT(element), NULL, NULL);
    } else if (!strcmp(name, "videocrop") && !g_object_get_data(G_OBJECT(element), "pendesk-crop")) {
        GstPad *pad = gst_element_get_static_pad(element, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                          crop_view, element, NULL);
        gst_object_unref(pad);
        g_object_set_data(G_OBJECT(element), "pendesk-crop", GINT_TO_POINTER(1));
    } else if (!strcmp(name, "queue") || !strcmp(name, "multiqueue")) {
        g_object_set(element, "max-size-buffers", 3u, "max-size-bytes", 0u,
                     "max-size-time", (guint64)(50 * GST_MSECOND), NULL);
    }
}

static void redirect(GObject *object, GParamSpec *property, gpointer data) {
    (void)property;
    (void)data;
    gchar *uri = NULL;
    g_object_get(object, "uri", &uri, NULL);
    if (uri && !strcmp(uri, "http://127.0.0.1:999/native"))
        g_object_set(object, "uri", "appsrc://pendesk", NULL);
    g_free(uri);
}

static gboolean added(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data) {
    (void)hint;
    (void)data;
    if (count != 3) return TRUE;
    GstElement *root = g_value_get_object(values);
    GstElement *element = g_value_get_object(values + 2);
    if (!GST_IS_PIPELINE(root)) return TRUE;
    if (!g_object_get_data(G_OBJECT(root), "pendesk-uri-hook") &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(root), "uri")) {
        g_object_set_data(G_OBJECT(root), "pendesk-uri-hook", GINT_TO_POINTER(1));
        g_signal_connect(root, "notify::uri", G_CALLBACK(redirect), NULL);
        redirect(G_OBJECT(root), NULL, NULL);
    }
    if (GST_IS_APP_SRC(element)) {
        gchar *uri = gst_uri_handler_get_uri(GST_URI_HANDLER(element));
        gboolean ours = uri && !strcmp(uri, "appsrc://pendesk");
        g_free(uri);
        if (!ours || g_object_get_data(G_OBJECT(element), "pendesk-source")) return TRUE;
        g_object_set_data(G_OBJECT(element), "pendesk-source", GINT_TO_POINTER(1));
        g_weak_ref_set(&pipeline, root);
        GstCaps *caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au,framerate=60/1");
        gst_app_src_set_caps(GST_APP_SRC(element), caps);
        gst_caps_unref(caps);
        g_object_set(element, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
                     "min-latency", (gint64)0, "max-latency", (gint64)0, NULL);
        struct source *source = g_new0(struct source, 1);
        source->fd = -1;
        g_weak_ref_init(&source->element, element);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 2, feed, source, release_source);
        GstIterator *iterator = gst_bin_iterate_recurse(GST_BIN(root));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
            tune(g_value_get_object(&item));
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(iterator);
    } else {
        GstElement *current = g_weak_ref_get(&pipeline);
        if (current == root) tune(element);
        if (current) gst_object_unref(current);
    }
    return TRUE;
}

void custom_init_jsapis(void) {
    registerCModuleLoader("vid", control_load);
    static gboolean initialized;
    gst_init(NULL, NULL);
    if (!initialized) {
        g_weak_ref_init(&pipeline, NULL);
        g_weak_ref_init(&sink, NULL);
        g_weak_ref_init(&flip, NULL);
        initialized = TRUE;
    }
    gpointer klass = g_type_class_ref(GST_TYPE_BIN);
    guint signal = g_signal_lookup("deep-element-added", GST_TYPE_BIN);
    GQuark key = g_quark_from_static_string("pendesk-video-hook");
    gulong previous = (gulong)g_type_get_qdata(GST_TYPE_BIN, key);
    if (previous) g_signal_remove_emission_hook(signal, previous);
    gulong hook = g_signal_add_emission_hook(signal, 0, added, NULL, NULL);
    g_type_set_qdata(GST_TYPE_BIN, key, (gpointer)hook);
    g_type_class_unref(klass);
}

int jsapi_check_unload(void) {
    return 1;
}
