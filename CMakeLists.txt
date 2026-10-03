cmake_minimum_required(VERSION 3.22)

project(rostrum VERSION 0.1.0 LANGUAGES CXX)

set(ROSTRUM_APP_ID "io.github.rostrum_audio.Rostrum")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(ROSTRUM_BUILD_TESTS "Build unit tests" ON)
option(ROSTRUM_BUILD_APP "Build the Kirigami app" ON)

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
if(ROSTRUM_BUILD_APP)
    # Found here so the imported QML plugin targets are visible to Qt's deferred finalizers.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Widgets)
endif()
find_package(PkgConfig REQUIRED)
find_package(tomlplusplus 3 REQUIRED)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.0)

add_compile_definitions(
    QT_NO_KEYWORDS
    ROSTRUM_VERSION="${PROJECT_VERSION}"
    ROSTRUM_APP_ID="${ROSTRUM_APP_ID}"
)

add_subdirectory(src/core)
add_subdirectory(src/pw)
add_subdirectory(src/engine)
add_subdirectory(tools/rostrum-probe)
add_subdirectory(tools/rostrum-graphtest)
if(ROSTRUM_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(ROSTRUM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
