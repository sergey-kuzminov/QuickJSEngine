cmake_minimum_required(VERSION 3.21)

project(speed LANGUAGES C CXX)

option(SKIP_QT_BENCHMARK "Skip the stock QJSEngine reference benchmark" OFF)

if(NOT SKIP_QT_BENCHMARK)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Core Qml)
else()
    find_package(Qt6 6.10 REQUIRED COMPONENTS Core)
endif()

qt_standard_project_setup()

# qt_standard_project_setup() defaults to C++17; the wrapper needs C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vendored quickjs-ng requires C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------
# QuickJS-NG (Next Generation), MIT license. Originally by
# Fabrice Bellard and Charlie Gordon.
# https://quickjs-ng.github.io/quickjs
# https://github.com/quickjs-ng/quickjs
# -------------------------------------------------------------------
file(GLOB QUICKJS_NG_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng/*.c")
file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp")

qt_add_executable(speed main.cpp
    ${QUICKJS_NG_SOURCES}
    ${WRAPPER_SOURCES}
)

target_include_directories(speed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Required by quickjs-ng on Linux/POSIX
target_compile_definitions(speed PRIVATE _GNU_SOURCE)

if(NOT SKIP_QT_BENCHMARK)
    target_link_libraries(speed PRIVATE Qt6::Qml)
else()
    target_compile_definitions(speed PRIVATE SKIP_QT_BENCHMARK)
    target_link_libraries(speed PRIVATE Qt6::Core)
endif()

# main.cpp reads process memory via psapi on Windows
if(WIN32)
    target_link_libraries(speed PRIVATE psapi)
endif()
