enable_language(C)

set(ROSTRUM_RNNOISE "auto" CACHE STRING
    "Noise suppression: auto (system RNNoise, else the pinned release), system, bundled or off")
set_property(CACHE ROSTRUM_RNNOISE PROPERTY STRINGS auto system bundled off)

set(rnnoise_library "")
if(ROSTRUM_RNNOISE STREQUAL "auto" OR ROSTRUM_RNNOISE STREQUAL "system")
    pkg_check_modules(RNNoise IMPORTED_TARGET rnnoise)
    if(RNNoise_FOUND)
        set(rnnoise_library PkgConfig::RNNoise)
        message(STATUS "Mic filters: system RNNoise ${RNNoise_VERSION}")
    elseif(ROSTRUM_RNNOISE STREQUAL "system")
        message(FATAL_ERROR "ROSTRUM_RNNOISE=system but pkg-config cannot find rnnoise")
    endif()
endif()

if(NOT rnnoise_library AND (ROSTRUM_RNNOISE STREQUAL "auto" OR ROSTRUM_RNNOISE STREQUAL "bundled"))
    include(FetchContent)
    # Offline builds can point FETCHCONTENT_SOURCE_DIR_RNNOISE at an unpacked copy.
    FetchContent_Declare(rnnoise
        URL https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
        URL_HASH SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(rnnoise)

    set(rnnoise_src ${rnnoise_SOURCE_DIR}/src)
    add_library(rostrum_rnnoise STATIC
        ${rnnoise_src}/denoise.c
        ${rnnoise_src}/rnn.c
        ${rnnoise_src}/pitch.c
        ${rnnoise_src}/kiss_fft.c
        ${rnnoise_src}/celt_lpc.c
        ${rnnoise_src}/nnet.c
        ${rnnoise_src}/nnet_default.c
        ${rnnoise_src}/parse_lpcnet_weights.c
        ${rnnoise_src}/rnnoise_data.c
        ${rnnoise_src}/rnnoise_tables.c)
    target_include_directories(rostrum_rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include PRIVATE ${rnnoise_src})
    target_compile_definitions(rostrum_rnnoise PRIVATE DISABLE_DEBUG_FLOAT)
    target_compile_options(rostrum_rnnoise PRIVATE -O2 -w)
    target_link_libraries(rostrum_rnnoise PRIVATE m)
    set_target_properties(rostrum_rnnoise PROPERTIES
        C_STANDARD 99
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        # Picks the SSE4.1 or AVX2 network kernels at run time, as upstream's build does.
        target_sources(rostrum_rnnoise PRIVATE
            ${rnnoise_src}/x86/x86_dnn_map.c
            ${rnnoise_src}/x86/x86cpu.c
            ${rnnoise_src}/x86/nnet_sse4_1.c
            ${rnnoise_src}/x86/nnet_avx2.c)
        target_compile_definitions(rostrum_rnnoise PRIVATE RNN_ENABLE_X86_RTCD CPU_INFO_BY_ASM)
        set_source_files_properties(${rnnoise_src}/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(${rnnoise_src}/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    endif()

    set(rnnoise_library rostrum_rnnoise)
    message(STATUS "Mic filters: bundled RNNoise 0.2")
    # BSD-3-Clause: binary copies carry the notice.
    install(FILES ${rnnoise_SOURCE_DIR}/COPYING
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rostrum
        RENAME RNNoise-COPYING)
endif()

if(NOT rnnoise_library)
    message(STATUS "Mic filters: built without noise suppression")
endif()

add_library(rostrum-dsp MODULE
    ladspa_abi.h
    dsp_common.h
    plugin.c
    highpass.c
    eq.c
    gate.c
    compressor.c
    limiter.c)
if(rnnoise_library)
    target_sources(rostrum-dsp PRIVATE denoise.c)
    target_compile_definitions(rostrum-dsp PRIVATE ROSTRUM_DSP_DENOISE)
    target_link_libraries(rostrum-dsp PRIVATE ${rnnoise_library})
endif()
target_link_libraries(rostrum-dsp PRIVATE m)
# Runs in the PipeWire daemon's realtime thread, so it is optimised in every build type.
target_compile_options(rostrum-dsp PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
# Loaded into the PipeWire daemon: export only ladspa_descriptor and fail the link on
# anything unresolved rather than at load time.
target_link_options(rostrum-dsp PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
set_target_properties(rostrum-dsp PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/rostrum)

install(TARGETS rostrum-dsp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/rostrum)

if(rnnoise_library)
    set(ROSTRUM_DSP_DENOISE ON PARENT_SCOPE)
else()
    set(ROSTRUM_DSP_DENOISE OFF PARENT_SCOPE)
endif()
