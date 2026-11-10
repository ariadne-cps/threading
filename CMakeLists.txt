cmake_minimum_required(VERSION 3.15)

project(Threading VERSION 3.0)

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT TARGET ariadne-build)
    find_package(Git REQUIRED)
    # Keep initial Build checkout output in English.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "LC_ALL=C"
                "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                submodule update --init -- ss/bui
        RESULT_VARIABLE _result)
    if(_result)
        message(FATAL_ERROR "Cannot initialize build submodule")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/ss/bui/cmake/ProjectOptions.cmake")
endif()

require_same_dependency_commit(Build CHAINS
    "build" "logging|build")

find_package(Threads REQUIRED)
require_same_dependency_commit(Logging CHAINS
    "logging" "benchmarking|logging")

ariadne_setup_module(threading)
target_link_libraries(threading INTERFACE Threads::Threads)
