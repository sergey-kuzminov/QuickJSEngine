cmake_minimum_required(VERSION 3.21)

project(example LANGUAGES C CXX)

option(USE_QT_JSENGINE "Build against the stock QJSEngine" OFF)

if(USE_QT_JSENGINE)
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
    # Required by quickjs-ng on Linux/POSIX
    target_compile_definitions(example PRIVATE _GNU_SOURCE)
    target_link_libraries(example PRIVATE Qt6::Core)
endif()
