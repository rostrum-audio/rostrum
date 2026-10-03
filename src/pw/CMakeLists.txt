add_library(rostrum_pw STATIC
    Graph.cpp Graph.h
    PwContext.cpp PwContext.h
)

target_include_directories(rostrum_pw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rostrum_pw PUBLIC Qt6::Core PkgConfig::PipeWire)
