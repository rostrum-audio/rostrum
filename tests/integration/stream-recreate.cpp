// rostrum-test-stream: a playback app for the private audio tests that can replace its stream.
//
// It plays a sine on one playback stream with the given application.name, like an ordinary app.
// When the control file holds a new request line, it destroys that stream and creates an identical
// one in the same loop iteration, as a browser does when it changes tracks or formats. PipeWire
// then sees the removal and the new node in one burst, so the new node can reuse the old one's ID.
//
//   rostrum-test-stream --app NAME --control FILE [--frequency HZ] [--no-restore-target]
//
// Prints "node <id>" each time a stream reaches the paused or streaming state, and
// "replaced <request>" after a replacement. Test tooling only.

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;

struct App
{
    pw_main_loop *loop = nullptr;
    pw_core *core = nullptr;
    pw_context *context = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::string app, control, last;
    double frequency = 440, phase = 0;
    bool reported = false, restoreTarget = true;
};

void createStream(App &a);

void onProcess(void *data)
{
    auto &a = *static_cast<App *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(a.stream);
    if (!b) {
        return;
    }
    spa_buffer *buf = b->buffer;
    auto *dst = static_cast<float *>(buf->datas[0].data);
    if (!dst) {
        return;
    }
    const uint32_t stride = sizeof(float) * kChannels;
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested && b->requested < frames) {
        frames = b->requested;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = float(0.1 * std::sin(a.phase));
        a.phase += 2 * M_PI * a.frequency / kRate;
        if (a.phase > 2 * M_PI) {
            a.phase -= 2 * M_PI;
        }
        for (int c = 0; c < kChannels; ++c) {
            *dst++ = v;
        }
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = frames * stride;
    pw_stream_queue_buffer(a.stream, b);
}

void onState(void *data, pw_stream_state, pw_stream_state state, const char *error)
{
    auto &a = *static_cast<App *>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "stream error: %s\n", error ? error : "");
        pw_main_loop_quit(a.loop);
        return;
    }
    if ((state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) && !a.reported) {
        std::printf("node %u\n", pw_stream_get_node_id(a.stream));
        std::fflush(stdout);
        a.reported = true;
    }
}

const pw_stream_events kEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = onState,
    .process = onProcess,
};

void createStream(App &a)
{
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, a.app.c_str(),
                                             PW_KEY_NODE_NAME, a.app.c_str(), nullptr);
    if (!a.restoreTarget) {
        // WirePlumber would otherwise put the new stream where it remembers the old one: only
        // Rostrum's router may then place it.
        pw_properties_set(props, "state.restore-target", "false");
    }
    a.stream = pw_stream_new(a.core, a.app.c_str(), props);
    a.reported = false;
    pw_stream_add_listener(a.stream, &a.listener, &kEvents, &a);
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    pw_stream_connect(a.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
                      params, 1);
}

void onPoll(void *data, uint64_t)
{
    auto &a = *static_cast<App *>(data);
    std::ifstream file(a.control);
    std::string request;
    std::getline(file, request);
    if (request.empty() || request == a.last) {
        return;
    }
    a.last = request;
    // Destroy and recreate before returning to the loop: both reach the server in one flush.
    spa_hook_remove(&a.listener);
    pw_stream_destroy(a.stream);
    createStream(a);
    std::printf("replaced %s\n", request.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    App a;
    for (int i = 1; i < argc; i += 2) {
        if (!std::strcmp(argv[i], "--no-restore-target")) {
            a.restoreTarget = false;
            --i;
            continue;
        }
        if (i + 1 >= argc) {
            break;
        }
        if (!std::strcmp(argv[i], "--app")) {
            a.app = argv[i + 1];
        } else if (!std::strcmp(argv[i], "--control")) {
            a.control = argv[i + 1];
        } else if (!std::strcmp(argv[i], "--frequency")) {
            a.frequency = std::atof(argv[i + 1]);
        }
    }
    if (a.app.empty() || a.control.empty()) {
        std::fprintf(stderr, "usage: %s --app NAME --control FILE [--frequency HZ] [--no-restore-target]\n", argv[0]);
        return 2;
    }
    { // Requests already in the file when the client starts are not replacements.
        std::ifstream file(a.control);
        std::getline(file, a.last);
    }
    pw_init(&argc, &argv);
    a.loop = pw_main_loop_new(nullptr);
    a.context = pw_context_new(pw_main_loop_get_loop(a.loop), nullptr, 0);
    a.core = pw_context_connect(a.context, nullptr, 0);
    if (!a.core) {
        std::fprintf(stderr, "cannot connect to PipeWire\n");
        return 2;
    }
    createStream(a);
    spa_source *timer = pw_loop_add_timer(pw_main_loop_get_loop(a.loop), onPoll, &a);
    timespec interval{0, 25 * 1000 * 1000};
    pw_loop_update_timer(pw_main_loop_get_loop(a.loop), timer, &interval, &interval, false);
    pw_main_loop_run(a.loop);
    pw_stream_destroy(a.stream);
    pw_core_disconnect(a.core);
    pw_context_destroy(a.context);
    pw_main_loop_destroy(a.loop);
    pw_deinit();
    return 0;
}
