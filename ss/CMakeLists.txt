if(NOT TARGET logging)
    ensure_submodule_initialized(logging)
    add_subdirectory(logging)
endif()
