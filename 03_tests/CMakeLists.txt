cmake_minimum_required(VERSION 3.21)

project(tests LANGUAGES C CXX)

# Enable AddressSanitizer + UndefinedBehaviorSanitizer (default, as in tests.pro).
# -fno-omit-frame-pointer keeps stack traces readable in sanitizer reports.
option(QUICKJS_SANITIZERS "Build tests with ASan/UBSan" ON)

# Serial test runs, stress mode (was: commented "DEFINES += SERIAL_TEST_RUNS=100").
# Enable with: cmake -B build -DSERIAL_TEST_RUNS=100
set(SERIAL_TEST_RUNS "" CACHE STRING "Run the suite N times serially (stress test)")

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Qml)

qt_standard_project_setup()

# Phase D (porting guide): C++20 after qt_standard_project_setup()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# quickjs-ng vendor C sources: C11 (gnu11) as before
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------
# QuickJS-NG (Next Generation), MIT license. Originally developed by:
# Fabrice Bellard and Charlie Gordon.
# https://quickjs-ng.github.io/quickjs
# https://github.com/quickjs-ng/quickjs
# -------------------------------------------------------------------
file(GLOB QUICKJS_NG_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng/*.c")
file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp")

qt_add_executable(tests main.cpp
    ${QUICKJS_NG_SOURCES}
    ${WRAPPER_SOURCES}
)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# The build fails without this (quickjs-ng requirement on Linux/POSIX):
target_compile_definitions(tests PRIVATE _GNU_SOURCE)

target_link_libraries(tests PRIVATE Qt6::Core Qt6::Qml)

if(SERIAL_TEST_RUNS)
    target_compile_definitions(tests PRIVATE SERIAL_TEST_RUNS=${SERIAL_TEST_RUNS})
endif()

if(QUICKJS_SANITIZERS)
    target_compile_options(tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tests PRIVATE -fsanitize=address,undefined)
endif()

# -O3 in release carried over from tests.pro (explicit flag, porting guide §6.4)
target_compile_options(tests PRIVATE
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-O3>
)
