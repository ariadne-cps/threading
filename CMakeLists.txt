cmake_minimum_required(VERSION 3.15)

project(Threading VERSION 1.0)

option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/submodules/configuration/cmake/ProjectOptions.cmake")
    message(FATAL_ERROR
        "Git submodules are not initialized. "
        "Run 'git submodule update --init --recursive' before configuring the project.")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/submodules/configuration/cmake/ProjectOptions.cmake")

require_initialized_submodules()

option(THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

setup_project()

if(COVERAGE AND THREAD_SANITIZER)
    message(FATAL_ERROR "COVERAGE and THREAD_SANITIZER cannot be enabled together.")
endif()

if(THREAD_SANITIZER)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR "ThreadSanitizer requires GCC or Clang on Unix-like systems.")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
    require_same_dependency_commit(Configuration
        REFERENCES
            "${PROJECT_SOURCE_DIR}|submodules/configuration"
            "${PROJECT_SOURCE_DIR}/submodules/logging|submodules/configuration"
            "${PROJECT_SOURCE_DIR}/submodules/utility|submodules/configuration"
    )
endif()

find_package(Threads REQUIRED)

if(NOT TARGET threading)
    add_subdirectory(submodules)

    add_library(threading INTERFACE)
    target_include_directories(threading INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/ariadne-threading>
    )
    target_link_libraries(threading INTERFACE logging utility Threads::Threads)

    add_subdirectory(src)
    target_link_libraries(THREADING_SRC PRIVATE threading)
endif()

setup_project_library(
    TARGET ariadne-threading
    OBJECTS
        THREADING_SRC
        LOGGING_SRC
        UTILITY_SRC
    LINK_LIBRARIES threading
)

setup_project_tests(
    EXCLUDE_REGEX "*/submodules/*"
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    get_target_property(LOGGING_SOURCE_DIR logging SOURCE_DIR)
    get_target_property(UTILITY_SOURCE_DIR utility SOURCE_DIR)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/threading"
        DESTINATION include/ariadne-threading
    )
    install(DIRECTORY "${LOGGING_SOURCE_DIR}/include/logging"
        DESTINATION include/ariadne-threading
    )
    install(DIRECTORY "${UTILITY_SOURCE_DIR}/include/utility"
        DESTINATION include/ariadne-threading
    )
endif()
