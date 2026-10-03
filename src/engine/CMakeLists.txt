add_library(rostrum_engine STATIC
    Engine.cpp Engine.h
    EngineMix.cpp
    NodeSpecs.cpp NodeSpecs.h
)

target_include_directories(rostrum_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_engine PUBLIC rostrum_core rostrum_pw)
