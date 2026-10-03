add_library(rostrum_core STATIC
    Autostart.cpp Autostart.h
    Paths.cpp Paths.h
    Requirements.cpp Requirements.h
    Model.cpp Model.h
    AppIdentity.cpp AppIdentity.h
    CrashReport.cpp CrashReport.h
    UpdateFeed.cpp UpdateFeed.h
    AppClassifier.cpp AppClassifier.h
    AppFacts.cpp AppFacts.h
    Ducking.cpp Ducking.h
    MicFilters.cpp MicFilters.h
    DspPlugin.cpp DspPlugin.h
    DesktopEntries.cpp DesktopEntries.h
    Remote.cpp Remote.h
    RuleExport.cpp RuleExport.h
    ScenePresets.cpp ScenePresets.h
    SceneToml.cpp SceneToml.h
    SceneStore.cpp SceneStore.h
    SceneTrash.cpp SceneTrash.h
    SceneHistory.cpp SceneHistory.h
    Settings.cpp Settings.h
    SettingsBackup.cpp SettingsBackup.h
    Volume.h
)

target_include_directories(rostrum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_core PUBLIC Qt6::Core PRIVATE tomlplusplus::tomlplusplus)

if(TARGET rostrum-dsp)
    set(dsp_defines
        ROSTRUM_DSP_BUILD_PATH="$<TARGET_FILE:rostrum-dsp>"
        ROSTRUM_BINARY_DIR="${CMAKE_BINARY_DIR}"
        ROSTRUM_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}"
        ROSTRUM_DSP_INSTALL_PATH="${CMAKE_INSTALL_FULL_LIBDIR}/rostrum/librostrum-dsp.so")
    if(ROSTRUM_DSP_DENOISE)
        list(APPEND dsp_defines ROSTRUM_DSP_DENOISE)
    endif()
    set_source_files_properties(DspPlugin.cpp PROPERTIES COMPILE_DEFINITIONS "${dsp_defines}")
    add_dependencies(rostrum_core rostrum-dsp)
endif()
