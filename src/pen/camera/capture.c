#define _POSIX_C_SOURCE 200809L
#include "io.h"
#include "stream.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <signal.h>
#include <unistd.h>

volatile sig_atomic_t alive = 1;
static void stop(int signal) { (void)signal; alive = 0; }

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    gst_init(NULL, NULL);
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "v4l2src name=camera io-mode=mmap ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! "
        "jpegenc quality=60 ! tee name=frames "
        "frames. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "appsink name=stream sync=false max-buffers=1 drop=true wait-on-eos=false "
        "frames. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! avimux ! filesink name=file", &error);
    if (!pipeline || error) {
        if (error) { g_printerr("%s\n", error->message); g_error_free(error); }
        if (pipeline) gst_object_unref(pipeline);
        return 1;
    }
    GstElement *camera = gst_bin_get_by_name(GST_BIN(pipeline), "camera");
    GstElement *file = gst_bin_get_by_name(GST_BIN(pipeline), "file");
    GstElement *stream = gst_bin_get_by_name(GST_BIN(pipeline), "stream");
    g_object_set(camera, "device", argv[1], NULL);
    g_object_set(file, "location", argv[2], NULL);
    gst_object_unref(camera);
    gst_object_unref(file);
    GstBus *bus = gst_element_get_bus(pipeline);
    int failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
    while (alive && !failed) {
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        if (message) { failed = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR; gst_message_unref(message); break; }
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(stream), 100 * GST_MSECOND);
        if (!sample) continue;
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            struct stream_packet packet = {.length = (uint32_t)map.size, .keyframe = 1,
                .timestamp = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ? GST_BUFFER_PTS(buffer) / GST_MSECOND : 0};
            failed = io_write_all(STDOUT_FILENO, &packet, sizeof(packet)) != 0 ||
                     io_write_all(STDOUT_FILENO, map.data, map.size) != 0;
            gst_buffer_unmap(buffer, &map);
        } else failed = 1;
        gst_sample_unref(sample);
    }
    gst_element_send_event(pipeline, gst_event_new_eos());
    GstMessage *eos = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (eos) gst_message_unref(eos);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(stream);
    gst_object_unref(pipeline);
    return failed;
}
