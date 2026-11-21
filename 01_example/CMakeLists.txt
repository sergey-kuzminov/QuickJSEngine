cmake_minimum_required(VERSION 3.21)

project(example LANGUAGES C CXX)

# Build against the stock QJSEngine instead of QQuickJSEngine
# (was: commented "DEFINES += USE_QT_JSENGINE" in example.pro).
option(USE_QT_JSENGINE "Build against the stock QJSEngine" OFF)

if(USE_QT_JSENGINE)
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

if(NOT USE_QT_JSENGINE)
    file(GLOB QUICKJS_NG_SOURCES CONFIGURE_DEPENDS
         LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng/*.c")
    file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS
         LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp")
endif()

if(USE_QT_JSENGINE)
    qt_add_executable(example main.cpp)
    target_compile_definitions(example PRIVATE USE_QT_JSENGINE)
    target_link_libraries(example PRIVATE Qt6::Qml)
else()
    qt_add_executable(example main.cpp
        ${QUICKJS_NG_SOURCES}
        ${WRAPPER_SOURCES}
    )
    target_include_directories(example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    # Required by quickjs-ng on Linux/POSIX (was: QMAKE_CFLAGS += -D_GNU_SOURCE)
    target_compile_definitions(example PRIVATE _GNU_SOURCE)
    target_link_libraries(example PRIVATE Qt6::Core)
endif()

# -O3 in release carried over from example.pro (explicit flag, porting guide §6.4)
target_compile_options(example PRIVATE
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>>:-O3>
    $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-O3>
)
