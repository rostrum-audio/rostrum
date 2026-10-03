cmake_minimum_required(VERSION 3.22)

project(rostrum VERSION 0.1.0 LANGUAGES CXX)

set(ROSTRUM_APP_ID "io.github._22Pizzas.Rostrum")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

option(ROSTRUM_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
find_package(tomlplusplus 3 REQUIRED)

add_compile_definitions(
    QT_NO_KEYWORDS
    ROSTRUM_VERSION="${PROJECT_VERSION}"
    ROSTRUM_APP_ID="${ROSTRUM_APP_ID}"
)

add_subdirectory(src/core)

if(ROSTRUM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
