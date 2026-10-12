cmake_minimum_required(VERSION 3.15)

project(Threading VERSION 1.0)

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ss/configuration/cmake/ProjectOptions.cmake")
    message(FATAL_ERROR
        "Git submodules are not initialized. "
        "Run 'git submodule update --init --recursive' before configuring the project.")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/ss/configuration/cmake/ProjectOptions.cmake")

require_initialized_submodules()

setup_project()

if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    require_same_dependency_commit(Configuration
        REFERENCES
            "${PROJECT_SOURCE_DIR}|ss/configuration"
            "${PROJECT_SOURCE_DIR}/ss/utility|ss/configuration"
            "${PROJECT_SOURCE_DIR}/ss/utility/ss/macro|ss/configuration"
            "${PROJECT_SOURCE_DIR}/ss/logging|ss/configuration"
            "${PROJECT_SOURCE_DIR}/ss/logging/ss/macro|ss/configuration"
    )
    require_same_dependency_commit(Macro
        REFERENCES
            "${PROJECT_SOURCE_DIR}/ss/utility|ss/macro"
            "${PROJECT_SOURCE_DIR}/ss/logging|ss/macro"
    )
endif()

find_package(Threads REQUIRED)

add_subdirectory(ss)

if(NOT TARGET threading)
    add_library(threading INTERFACE)
    target_include_directories(threading INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/ariadne-threading>
    )
    target_link_libraries(threading INTERFACE logging utility Threads::Threads)

    add_subdirectory(src)
    target_link_libraries(THREADING_SRC PRIVATE threading)
endif()

ariadne_register_public_headers(
    TARGET threading
    DIRECTORIES "${PROJECT_SOURCE_DIR}/include/threading"
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
