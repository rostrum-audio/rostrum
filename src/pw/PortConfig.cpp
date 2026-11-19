#include "pw/PortConfig.h"

#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/port-config.h>

namespace rostrum::pw {

spa_pod *buildDspPortConfig(spa_pod_builder *b, bool output, int channels)
{
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    // In DSP mode audioconvert discards the rate and the ports follow the graph clock, but
    // PipeWire 1.4 refuses a format without one (EINVAL), so the node never gets its ports.
    info.rate = 48000;
    if (channels <= 1) {
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig);
    spa_pod_builder_add(b, SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(output ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT),
                        SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp), 0);
    spa_pod_builder_prop(b, SPA_PARAM_PORT_CONFIG_format, 0);
    spa_format_audio_raw_build(b, SPA_PARAM_Format, &info);
    return static_cast<spa_pod *>(spa_pod_builder_pop(b, &f));
}

} // namespace rostrum::pw
