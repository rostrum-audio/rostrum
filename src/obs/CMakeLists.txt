add_library(rostrum_obs STATIC
    ObsModel.cpp ObsModel.h
    ObsConfig.cpp ObsConfig.h
    ObsPlan.cpp ObsPlan.h
    SceneCollection.cpp SceneCollection.h
    ObsClient.cpp ObsClient.h
    ObsLive.cpp ObsLive.h
    ObsCaptures.cpp ObsCaptures.h
    ObsStatus.cpp ObsStatus.h
    Readiness.cpp Readiness.h
)

target_include_directories(rostrum_obs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_obs PUBLIC rostrum_core rostrum_pw Qt6::Core Qt6::WebSockets)
