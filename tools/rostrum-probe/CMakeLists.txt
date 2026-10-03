add_executable(rostrum-probe main.cpp)
target_link_libraries(rostrum-probe PRIVATE rostrum_core PkgConfig::PipeWire)
install(TARGETS rostrum-probe RUNTIME DESTINATION bin)
