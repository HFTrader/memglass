cmake_minimum_required(VERSION 3.16)
project(memglass VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MEMGLASS_BUILD_EXAMPLES "Build examples" ON)
option(MEMGLASS_BUILD_TESTS "Build tests" ON)
option(MEMGLASS_BUILD_GENERATOR "Build memglass-gen tool" ON)
option(MEMGLASS_BUILD_WEB "Build memglass with web server support" ON)

# Find dependencies
find_package(fmt QUIET)
if(NOT fmt_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 10.2.1
    )
    FetchContent_MakeAvailable(fmt)
endif()

# cpp-httplib for web server support
if(MEMGLASS_BUILD_WEB)
    find_package(httplib QUIET)
    if(NOT httplib_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG v0.15.3
        )
        FetchContent_MakeAvailable(httplib)
    endif()
endif()

# Core library
add_library(memglass
    src/memglass.cpp
    src/observer.cpp
    src/allocator.cpp
    src/registry.cpp
    src/platform/shm_posix.cpp
)

target_include_directories(memglass
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(memglass
    PUBLIC
        fmt::fmt
    PRIVATE
        pthread
        rt
)

target_compile_features(memglass PUBLIC cxx_std_20)

# memglass CLI tool (generic observer/visualizer)
add_executable(memglass-cli tools/memglass.cpp)
target_link_libraries(memglass-cli PRIVATE memglass)
if(MEMGLASS_BUILD_WEB)
    target_link_libraries(memglass-cli PRIVATE httplib::httplib)
    target_compile_definitions(memglass-cli PRIVATE MEMGLASS_WEB_ENABLED)
endif()
set_target_properties(memglass-cli PROPERTIES OUTPUT_NAME memglass)

# memglass-gen tool
if(MEMGLASS_BUILD_GENERATOR)
    find_package(Clang QUIET)
    if(NOT Clang_FOUND)
        # Try to find libclang manually
        find_library(LIBCLANG_LIBRARY NAMES clang clang-18 clang-17 clang-16 clang-15 clang-14
            PATHS /usr/lib /usr/lib/llvm-18/lib /usr/lib/llvm-17/lib /usr/lib/llvm-16/lib /usr/lib/llvm-15/lib /usr/lib/llvm-14/lib)
        find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
            PATHS /usr/include /usr/lib/llvm-18/include /usr/lib/llvm-17/include /usr/lib/llvm-16/include /usr/lib/llvm-15/include /usr/lib/llvm-14/include)
    endif()
    if(Clang_FOUND OR LIBCLANG_LIBRARY)
        add_subdirectory(tools/memglass-gen)
    else()
        message(FATAL_ERROR "libclang not found. Install libclang-dev or set -DMEMGLASS_BUILD_GENERATOR=OFF")
    endif()
endif()

# Examples
if(MEMGLASS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(MEMGLASS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install
include(GNUInstallDirs)
install(TARGETS memglass
    EXPORT memglassTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/memglass DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
