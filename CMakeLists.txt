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

include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/submodules/logging/include)
include_directories(${PROJECT_SOURCE_DIR}/submodules/utility/include)

if(NOT TARGET threading)
    add_subdirectory(src)
    add_subdirectory(submodules)

    add_library(threading INTERFACE)
    target_include_directories(threading INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/submodules/logging/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/submodules/utility/include>
        $<INSTALL_INTERFACE:include/ariadne-threading>
    )
    target_link_libraries(threading INTERFACE logging utility Threads::Threads)
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    add_library(ariadne-threading SHARED
        $<TARGET_OBJECTS:THREADING_SRC>
        $<TARGET_OBJECTS:LOGGING_SRC>
        $<TARGET_OBJECTS:UTILITY_SRC>
    )
    target_link_libraries(ariadne-threading PUBLIC threading)

    setup_standalone_project_tests(
        TARGET ariadne-threading
        TEST_DIRECTORY test
        TEST_TARGET tests
        SOURCES
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src"
        EXCLUDE_REGEX "*/submodules/*"
    )

    install(TARGETS ariadne-threading
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/threading"
        DESTINATION include/ariadne-threading
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/submodules/logging/include/logging"
        DESTINATION include/ariadne-threading
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/submodules/utility/include/utility"
        DESTINATION include/ariadne-threading
    )
endif()
