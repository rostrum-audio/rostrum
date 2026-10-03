add_library(rostrum_core STATIC
    Paths.cpp Paths.h
    Requirements.cpp Requirements.h
    Model.cpp Model.h
    Volume.h
)

target_include_directories(rostrum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_core PUBLIC Qt6::Core PRIVATE tomlplusplus::tomlplusplus)
