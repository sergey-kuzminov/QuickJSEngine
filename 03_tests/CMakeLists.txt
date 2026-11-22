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
