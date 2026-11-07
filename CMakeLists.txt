cmake_minimum_required(VERSION 3.15)

project(Threading VERSION 1.0)

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

setup_project()

if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    # Child commits are internally consistent; check direct occurrences.
    # Cross-branch consistency checks remain explicit where needed.
    require_same_dependency_commit(Build CHAINS
        "build" "logging|build" "testing|build")
    require_same_dependency_commit(Testing CHAINS
        "testing" "logging|testing")
endif()

find_package(Threads REQUIRED)

add_subdirectory(ss)

if(NOT TARGET threading)
    add_library(threading INTERFACE)
    target_include_directories(threading INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/ariadne-threading>
    )
    target_link_libraries(threading INTERFACE logging Threads::Threads)

    add_subdirectory(src)
    target_link_libraries(THREADING_SRC PRIVATE threading)
endif()

ariadne_register_public_headers(
    TARGET threading
    DIRECTORIES "${PROJECT_SOURCE_DIR}/include/threading"
    FILES "${PROJECT_SOURCE_DIR}/include/ariadne-threading.hpp"
)

setup_project_library(
    TARGET ariadne-threading
    OBJECTS THREADING_SRC
    LINK_LIBRARIES threading
)

setup_standalone_project_tests(
    TARGET ariadne-threading
    TEST_DIRECTORY tests
    TEST_TARGET tests
    SOURCES
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    ariadne_install_dependency_bundle(
        TARGET threading
        DESTINATION include/ariadne-threading
    )
endif()
