cmake_minimum_required(VERSION 3.21)

project(leakgate LANGUAGES C CXX)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core)

qt_standard_project_setup()

# Phase D (porting guide): C++20 after qt_standard_project_setup()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# quickjs-ng vendor C sources: C11 (gnu11) as before
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB QUICKJS_NG_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng/*.c")
file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp")

qt_add_executable(leakgate main.cpp
    ${QUICKJS_NG_SOURCES}
    ${WRAPPER_SOURCES}
)

target_include_directories(leakgate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Required by quickjs-ng on Linux/POSIX (was: QMAKE_CFLAGS += -D_GNU_SOURCE)
target_compile_definitions(leakgate PRIVATE _GNU_SOURCE)

target_link_libraries(leakgate PRIVATE Qt6::Core)

# ASan: make sure LTO stays disabled — it is (nothing enables IPO here;
# porting guide §6.4: no LTO/IPO in the CMake build). Sanitizer variant
# (was a commented block in leakgate.pro):
# target_compile_options(leakgate PRIVATE
#     -fsanitize=address -fno-omit-frame-pointer -g)
# target_link_options(leakgate PRIVATE -fsanitize=address)

# -O3 in release carried over from leakgate.pro (explicit flag, porting guide §6.4)
target_compile_options(leakgate PRIVATE
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-O3>
)
