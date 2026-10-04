cmake_minimum_required(VERSION 3.22)

project(rostrum VERSION 0.1.0 LANGUAGES CXX)

set(ROSTRUM_APP_ID "dev.getrostrum.Rostrum")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(ROSTRUM_BUILD_TESTS "Build unit tests" ON)
option(ROSTRUM_AUDIO_INTEGRATION_TESTS "Run isolated PipeWire/WirePlumber safety tests" OFF)
if(ROSTRUM_AUDIO_INTEGRATION_TESTS AND NOT ROSTRUM_BUILD_TESTS)
    message(FATAL_ERROR "Audio integration tests require ROSTRUM_BUILD_TESTS")
endif()
option(ROSTRUM_BUILD_APP "Build the Kirigami app" ON)
set(ROSTRUM_UPDATE_URL "https://getrostrum.dev/releases/latest.json" CACHE STRING
    "Release feed the update check reads (Rostrum feed or a GitHub latest-release URL)")
option(ROSTRUM_WITH_SENTRY "Capture crashes with sentry-native for crash reports (official builds)" OFF)
set(ROSTRUM_SENTRY_DSN "" CACHE STRING
    "Sentry DSN crash reports go to; without one, crash reporting is hidden")

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core WebSockets)
if(ROSTRUM_BUILD_APP)
    # Found here so the imported QML plugin targets are visible to Qt's deferred finalizers.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Widgets DBus Network)
endif()
find_package(PkgConfig REQUIRED)
find_package(tomlplusplus 3 REQUIRED)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.0)

add_compile_definitions(
    QT_NO_KEYWORDS
    ROSTRUM_VERSION="${PROJECT_VERSION}"
    ROSTRUM_APP_ID="${ROSTRUM_APP_ID}"
)

option(ROSTRUM_BUILD_DSP "Build the mic filter plugin (loaded into PipeWire)" ON)
set(ROSTRUM_DSP_DENOISE OFF)
if(ROSTRUM_BUILD_DSP)
    add_subdirectory(src/dsp)
endif()

add_subdirectory(src/core)
add_subdirectory(src/pw)
add_subdirectory(src/engine)
add_subdirectory(src/obs)
add_subdirectory(tools/rostrum-probe)
add_subdirectory(tools/rostrum-graphtest)
add_subdirectory(tools/rostrum-obs)
if(ROSTRUM_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(ROSTRUM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
