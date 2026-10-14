cmake_minimum_required(VERSION 3.15)

project(Threading VERSION 1.0)

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT TARGET ariadne-configuration)
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                    submodule update --init -- ss/configuration
                    RESULT_VARIABLE _result)
    if(_result)
        message(FATAL_ERROR "Cannot initialize configuration submodule")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/ss/configuration/cmake/ProjectOptions.cmake")
endif()

setup_project()

if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    require_same_dependency_commit(Configuration
        CHAINS
            "configuration"
            "logging|configuration"
            "logging|utility|configuration"
            "logging|utility|macro|configuration"
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
    target_link_libraries(threading INTERFACE logging Threads::Threads)

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
