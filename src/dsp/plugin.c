#include "dsp_common.h"

static const LADSPA_Descriptor *const descriptors[] = {
    &rd_highpass_descriptor,
    &rd_eq_descriptor,
    &rd_gate_descriptor,
    &rd_compressor_descriptor,
    &rd_limiter_descriptor,
#ifdef ROSTRUM_DSP_DENOISE
    &rd_denoise_descriptor,
#endif
};

__attribute__((visibility("default"))) const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    return index < sizeof(descriptors) / sizeof(descriptors[0]) ? descriptors[index] : NULL;
}
