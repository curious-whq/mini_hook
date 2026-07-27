cmake_minimum_required(VERSION 3.10)
project(mini_malloc_free_hook C)

set(MINI_REPLAY_MAPPING_SIZE "3221225472ULL" CACHE STRING
    "Replay BIN mapping size in bytes; use 6442450944ULL for 86M events")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(mini_malloc_free_hook SHARED malloc_free_hook.c)
set(MINI_TEST_REPLAY_PATH "${CMAKE_BINARY_DIR}/mini_replay_test.bin")
file(REMOVE "${MINI_TEST_REPLAY_PATH}")
target_compile_definitions(mini_malloc_free_hook PRIVATE
    "REPLAY_LOG_PATH=\"${MINI_TEST_REPLAY_PATH}\""
    "REPLAY_MAPPING_SIZE=${MINI_REPLAY_MAPPING_SIZE}"
)
target_compile_options(mini_malloc_free_hook PRIVATE
    -fPIC
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(mini_malloc_free_hook PRIVATE dl pthread)
target_link_options(mini_malloc_free_hook PRIVATE "-Wl,--no-undefined")

add_library(mini_malloc_hole_hook SHARED malloc_hole_hook.c)
target_compile_options(mini_malloc_hole_hook PRIVATE
    -O3
    -DNDEBUG
    -fPIC
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(mini_malloc_hole_hook PRIVATE dl)
target_link_options(mini_malloc_hole_hook PRIVATE "-Wl,--no-undefined")

add_executable(mini_hook_smoke_test smoke_test.c)
target_link_libraries(mini_hook_smoke_test PRIVATE dl pthread)
add_executable(mini_mstress mstress.c)
target_link_libraries(mini_mstress PRIVATE pthread)
target_compile_options(mini_mstress PRIVATE -O3 -DNDEBUG -Wall -Wextra -Werror)
add_executable(mini_mstress_bench mstress.c)
target_link_libraries(mini_mstress_bench PRIVATE pthread)
target_compile_definitions(mini_mstress_bench PRIVATE
    MINI_MSTRESS_NO_FINAL_SLEEP=1
)
target_compile_options(mini_mstress_bench PRIVATE
    -O3
    -DNDEBUG
    -Wall
    -Wextra
    -Werror
)
add_executable(mini_replay_dump replay_dump.c)
add_executable(mini_bin_to_rply bin_to_rply.c)
add_executable(mini_rply_to_txt rply_to_txt.c)
add_executable(mini_replay_main replay_main.c)
target_link_libraries(mini_replay_main PRIVATE pthread)
target_compile_options(mini_replay_main PRIVATE
    -O3
    -DNDEBUG
    -Wall
    -Wextra
    -Werror
)

# This target is a compile-time portability check for the tiny runtime.  A
# runnable standalone ELF is produced by trace_to_native.py, which appends its
# offline-compiled operation image to this runtime.
add_executable(mini_native_replay_runtime native_replay_runtime.c)
target_link_libraries(mini_native_replay_runtime PRIVATE pthread)
target_compile_options(mini_native_replay_runtime PRIVATE
    -O3
    -DNDEBUG
    -Wall
    -Wextra
    -Werror
)

# Compile-time portability check for the compact statistical workload runtime.
add_executable(mini_synthetic_mstress_runtime synthetic_mstress_runtime.c)
target_link_libraries(mini_synthetic_mstress_runtime PRIVATE pthread)
target_compile_options(mini_synthetic_mstress_runtime PRIVATE
    -O3
    -DNDEBUG
    -Wall
    -Wextra
    -Werror
)

foreach(tool
    mini_hook_smoke_test
    mini_replay_dump
    mini_bin_to_rply
    mini_rply_to_txt
)
    target_compile_options(${tool} PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endforeach()

enable_testing()
add_test(
    NAME mini_hook_smoke_test
    COMMAND
        ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:mini_malloc_free_hook>"
        $<TARGET_FILE:mini_hook_smoke_test>
)
add_test(
    NAME mini_malloc_hole_smoke_test
    COMMAND
        ${CMAKE_COMMAND} -E env
        "MINI_HOLE_OUTPUT_DIR=${CMAKE_BINARY_DIR}"
        "MINI_HOLE_INTERVAL_SEC=1"
        "LD_PRELOAD=$<TARGET_FILE:mini_malloc_hole_hook>"
        $<TARGET_FILE:mini_hook_smoke_test>
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME mini_native_replay_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/native_replay_test.py
    )
    add_test(
        NAME mini_synthetic_mstress_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/synthetic_mstress_test.py
    )
endif()
