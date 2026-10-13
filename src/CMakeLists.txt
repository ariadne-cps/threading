set(LIBRARY_NAME THREADING_SRC)

add_library(${LIBRARY_NAME} OBJECT
        command_line.cpp
        thread.cpp
        buffered_thread.cpp
        thread_pool.cpp
        workload_advancement.cpp
        thread_manager.cpp
        )
