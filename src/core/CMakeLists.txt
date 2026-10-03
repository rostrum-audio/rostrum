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
    DesktopEntries.cpp DesktopEntries.h
    Remote.cpp Remote.h
    RuleExport.cpp RuleExport.h
    ScenePresets.cpp ScenePresets.h
    SceneToml.cpp SceneToml.h
    SceneStore.cpp SceneStore.h
    Settings.cpp Settings.h
    Volume.h
)

target_include_directories(rostrum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_core PUBLIC Qt6::Core PRIVATE tomlplusplus::tomlplusplus)
