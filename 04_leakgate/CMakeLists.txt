cmake_minimum_required(VERSION 3.21)

project(leakgate LANGUAGES C CXX)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core)

qt_standard_project_setup()

# qt_standard_project_setup() defaults to C++17; the wrapper needs C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vendored quickjs-ng requires C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB QUICKJS_NG_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng/*.c")
file(GLOB WRAPPER_SOURCES CONFIGURE_DEPENDS
     LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.cpp")

# Vendor build shape: upstream quickjs-ng compiled optimized and without
# sanitizers even in Debug, so its interpreter frames stay release-sized
# and the JS stack-guard window stays healthy on fat Debug builds
# (see 03_tests/CMakeLists.txt for the full rationale).
option(QUICKJS_SLIM_VENDOR "Compile the vendored quickjs-ng optimized and without sanitizers (Debug too)" ON)
if(QUICKJS_SLIM_VENDOR AND NOT MSVC)
    set_source_files_properties(${QUICKJS_NG_SOURCES} PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O1>;-fno-sanitize=address,undefined")
endif()

qt_add_executable(leakgate main.cpp
    ${QUICKJS_NG_SOURCES}
    ${WRAPPER_SOURCES}
)

target_include_directories(leakgate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../quickjs-ng
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Required by quickjs-ng on Linux/POSIX
target_compile_definitions(leakgate PRIVATE _GNU_SOURCE)

target_link_libraries(leakgate PRIVATE Qt6::Core)

# Stack reserve: the JS stack guard needs a usable window below the entry
# machinery (see 03_tests/CMakeLists.txt); 2 MiB MinGW defaults starve it
# on Debug/sanitizer builds. 16 MiB is cheap insurance.
# Windows only: Linux stacks default to 8 MiB and the PE-only
# --stack flag does not exist for ELF linkers.
if(WIN32)
    if(MSVC)
        target_link_options(leakgate PRIVATE "/STACK:16777216")
    else()
        target_link_options(leakgate PRIVATE "-Wl,--stack,16777216")
    endif()
endif()
