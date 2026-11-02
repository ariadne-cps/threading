# Tests use the framework pinned directly by Threading. Reuse it if a
# parent project has already registered the matching target.
if(NOT TARGET ariadne-testing)
    ariadne_add_submodule(testing)
endif()

set(UNIT_TESTS
    test_buffer
    test_buffered_thread
    test_command_line
    test_thread
    test_thread_pool
    test_thread_manager
    test_workload_advancement
    test_workload
    test_task_group
    test_work_stealing_policy
    test_umbrella
    test_parallel
    test_coordination
)

foreach(TEST ${UNIT_TESTS})
    add_executable(${TEST} ${TEST}.cpp)
    target_link_libraries(${TEST} ariadne-threading threading ariadne-testing)
    add_test(NAME ${TEST} COMMAND $<TARGET_FILE:${TEST}> --concise)
    # Bounded observations cannot cancel a truly stuck worker.
    set_tests_properties(${TEST} PROPERTIES TIMEOUT 90)
    if(WIN32)
        set_tests_properties(${TEST} PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:ariadne-threading>"
            TIMEOUT 120
        )
    endif()
endforeach()

add_custom_target(tests)
add_dependencies(tests ${UNIT_TESTS})
