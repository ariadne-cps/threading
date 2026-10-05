set(LIBRARY_NAME THREADING_SRC)

add_library(${LIBRARY_NAME} OBJECT
        thread.cpp
        buffered_thread.cpp
        thread_pool.cpp
        workload_advancement.cpp
        thread_manager.cpp
        )

