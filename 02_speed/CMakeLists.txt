cmake_minimum_required(VERSION 3.21)

project(speed LANGUAGES C CXX)

# A/B without the stock QJSEngine reference
# (was: commented "DEFINES += SKIP_QT_BENCHMARK" in speed.pro)
option(SKIP_QT_BENCHMARK "Skip the stock QJSEngine reference benchmark" OFF)

if(NOT SKIP_QT_BENCHMARK)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Core Qml)
else()
    find_package(Qt6 6.10 REQUIRED COMPONENTS Core)
endif()

qt_standard_project_setup()

# Phase D (porting guide): C++20 after qt_standard_project_setup()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# quickjs-ng vendor C sources: C11 (gnu11) as before
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

# Required by quickjs-ng on Linux/POSIX (was: QMAKE_CFLAGS += -D_GNU_SOURCE)
target_compile_definitions(speed PRIVATE _GNU_SOURCE)

if(NOT SKIP_QT_BENCHMARK)
    target_link_libraries(speed PRIVATE Qt6::Qml)
else()
    target_compile_definitions(speed PRIVATE SKIP_QT_BENCHMARK)
    target_link_libraries(speed PRIVATE Qt6::Core)
endif()

# -O3 in release carried over from speed.pro (explicit flag, porting guide §6.4)
target_compile_options(speed PRIVATE
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-O3>
)

# Was win32:LIBS += -lpsapi
if(WIN32)
    target_link_libraries(speed PRIVATE psapi)
endif()

# ASan (was a commented block in speed.pro)
# set_target_properties(speed PROPERTIES
#     COMPILE_OPTIONS "-fsanitize=address;-fno-omit-frame-pointer;-g")
# target_link_options(speed PRIVATE -fsanitize=address)
