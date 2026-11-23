cmake_minimum_required(VERSION 3.21)

project(tests LANGUAGES C CXX)

# -fno-omit-frame-pointer keeps sanitizer stack traces readable
option(QUICKJS_SANITIZERS "Build tests with ASan/UBSan" ON)

# Stress mode: run the suite N times serially, e.g. -DSERIAL_TEST_RUNS=100
set(SERIAL_TEST_RUNS "" CACHE STRING "Run the suite N times serially (stress test)")

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Qml)

qt_standard_project_setup()

# qt_standard_project_setup() defaults to C++17; the wrapper needs C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vendored quickjs-ng requires C11
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

# -------------------------------------------------------------------
# Vendor build shape (upstream quickjs-ng, not ours to debug at -O0)
# -------------------------------------------------------------------
# Compile the vendored engine optimized and without sanitizers even in
# Debug: its interpreter frames (JS_CallInternal and friends) then stay
# release-sized (~2-3 KiB instead of ~20-40 KiB each under -O0+ASan
# redzones), which shrinks the calibrated entry-machinery allowance, the
# frame-hop overshoot past the guard line, and the stack-overflow throw
# path on every entry. The wrapper keeps full ASan/UBSan coverage and the
# quickjs heap stays covered by the global malloc interceptors, so heap
# diagnostics are not lost. Set -DQUICKJS_SLIM_VENDOR=OFF to restore full
# Debug sanitization of the vendor.
option(QUICKJS_SLIM_VENDOR "Compile the vendored quickjs-ng optimized and without sanitizers (Debug too)" ON)
if(QUICKJS_SLIM_VENDOR AND NOT MSVC)
    set_source_files_properties(${QUICKJS_NG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O1>;-fno-sanitize=address,undefined")
endif()

qt_add_executable(tests main.cpp
    ${QUICKJS_NG_SOURCES}
    ${WRAPPER_SOURCES}
)

# Module file embedded as a Qt resource for the importModule() qrc tests.
# qt_add_resources() generates the .qrc and compiles it under the name
# "qjsmodules"; the module is importable from ":/qjsmodules/modules/embedded.mjs".
qt_add_resources(tests "qjsmodules"
    FILES modules/embedded.mjs
    PREFIX "/qjsmodules"
)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Required by quickjs-ng on Linux/POSIX
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

# -------------------------------------------------------------------
# Stack reserve of the executable
# -------------------------------------------------------------------
# The JS stack guard needs a usable window below the entry machinery:
#   (SP - stack bottom) >= machinery allowance + JS recursion share + margin.
# ASan/Debug C frames are 10-40x their release size (measured ~340 KiB of
# legitimate machinery below evaluate() on MinGW clang Debug+ASan), so the
# 2 MiB MinGW-w64 default reserve leaves <450 KiB at the entry and the
# window degenerates: the guard then false-fires inside legitimate machinery
# (promise jobs: false "Maximum call stack size exceeded" RangeErrors from
# Promise combinators) and the stack-overflow throw path runs past the real
# bottom (hard ASan crash in the "Limit: Stack Overflow" recursion test).
# 16 MiB keeps the window healthy in every supported configuration; the
# PE reserve also becomes the default stack of CreateThread(0)/QThread.
# Windows only: Linux stacks default to 8 MiB and the PE-only --stack
# flag does not exist for ELF linkers.
if(WIN32)
    if(MSVC)
        target_link_options(tests PRIVATE "/STACK:16777216")
    else()
        target_link_options(tests PRIVATE "-Wl,--stack,16777216")
    endif()
endif()
