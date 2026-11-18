#pragma once

#include <spa/pod/builder.h>

namespace rostrum::pw {

// The PortConfig param that puts one side of an audio.convert node into DSP mode with 1 (MONO) or
// 2 (FL FR) channels. Returns nullptr if the builder runs out of space.
spa_pod *buildDspPortConfig(spa_pod_builder *b, bool output, int channels);

} // namespace rostrum::pw
