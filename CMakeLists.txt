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
target_compile_definitions(mini_malloc_hole_hook PRIVATE
    MINI_HOLE_ATOMIC_STATS=1
    MINI_HOLE_NO_WRITER=1
    MINI_HOLE_OPPORTUNISTIC_SNAPSHOT=1
)
add_library(mini_malloc_hole_test_hook SHARED malloc_hole_hook.c)
target_compile_definitions(mini_malloc_hole_test_hook PRIVATE
    MINI_HOLE_ATOMIC_STATS=1
    MINI_HOLE_NO_WRITER=1
    MINI_HOLE_OPPORTUNISTIC_SNAPSHOT=1
    MINI_HOLE_INTERVAL_SEC=604800U
    MINI_HOLE_LIVE_SAMPLE_RATE=1U
    MINI_HOLE_CLOCK_CHECK_EVERY=1U
    MINI_HOLE_LIVE_CAPACITY=1024U
)
add_library(mini_malloc_hole_no_writer SHARED malloc_hole_hook.c)
target_compile_definitions(mini_malloc_hole_no_writer PRIVATE
    MINI_HOLE_NO_WRITER=1
)
add_library(mini_malloc_hole_atomic_no_writer SHARED malloc_hole_hook.c)
target_compile_definitions(mini_malloc_hole_atomic_no_writer PRIVATE
    MINI_HOLE_ATOMIC_STATS=1
    MINI_HOLE_NO_WRITER=1
)
add_library(mini_malloc_hole_passthrough SHARED malloc_hole_passthrough.c)
add_library(
    mini_malloc_hole_all_api_passthrough SHARED
    malloc_hole_all_api_passthrough.c
)
add_library(mini_malloc_hole_probe SHARED malloc_hole_hook.c)
target_compile_definitions(mini_malloc_hole_probe PRIVATE
    MINI_HOLE_LOADER_PROBE=1
)

foreach(hole_target
    mini_malloc_hole_hook
    mini_malloc_hole_test_hook
    mini_malloc_hole_no_writer
    mini_malloc_hole_atomic_no_writer
    mini_malloc_hole_passthrough
    mini_malloc_hole_all_api_passthrough
    mini_malloc_hole_probe
)
    target_compile_options(${hole_target} PRIVATE
        -O3
        -DNDEBUG
        -fPIC
        -Wall
        -Wextra
        -Werror
    )
    target_link_options(${hole_target} PRIVATE "-Wl,--no-undefined")
endforeach()
target_link_libraries(mini_malloc_hole_hook PRIVATE dl)
target_link_libraries(mini_malloc_hole_test_hook PRIVATE dl)
target_link_libraries(mini_malloc_hole_no_writer PRIVATE dl pthread)
target_link_libraries(mini_malloc_hole_atomic_no_writer PRIVATE dl)
target_link_libraries(mini_malloc_hole_passthrough PRIVATE dl)
target_link_libraries(mini_malloc_hole_all_api_passthrough PRIVATE dl)

add_executable(mini_hook_smoke_test smoke_test.c)
target_link_libraries(mini_hook_smoke_test PRIVATE dl pthread)
add_executable(mini_hole_fork_test hole_fork_test.c)
target_compile_options(mini_hole_fork_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
add_executable(
    mini_hole_sparse_periodic_test
    hole_sparse_periodic_test.c
)
target_compile_options(mini_hole_sparse_periodic_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
add_executable(mini_hole_live_test hole_live_test.c)
target_link_libraries(mini_hole_live_test PRIVATE dl)
target_compile_options(mini_hole_live_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
add_executable(
    mini_appspawndf_live_scope_test
    appspawndf_live_scope_test.c
)
target_link_libraries(mini_appspawndf_live_scope_test PRIVATE dl)
target_compile_options(mini_appspawndf_live_scope_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
add_executable(
    mini_hole_live_capacity_test
    hole_live_capacity_test.c
)
target_link_libraries(mini_hole_live_capacity_test PRIVATE dl)
target_compile_options(mini_hole_live_capacity_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
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
        "LD_PRELOAD=$<TARGET_FILE:mini_malloc_hole_hook>"
        $<TARGET_FILE:mini_hook_smoke_test>
)
add_test(
    NAME mini_malloc_hole_fork_test
    COMMAND
        ${CMAKE_COMMAND} -E env
        "MINI_HOLE_OUTPUT_DIR=${CMAKE_BINARY_DIR}"
        "LD_PRELOAD=$<TARGET_FILE:mini_malloc_hole_hook>"
        $<TARGET_FILE:mini_hole_fork_test>
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME mini_malloc_hole_live_test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/hole_live_test.py
            $<TARGET_FILE:mini_malloc_hole_test_hook>
            $<TARGET_FILE:mini_hole_live_test>
    )
    add_test(
        NAME mini_malloc_hole_appspawndf_scope_test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/appspawndf_live_scope_test.py
            $<TARGET_FILE:mini_malloc_hole_test_hook>
            $<TARGET_FILE:mini_appspawndf_live_scope_test>
    )
    add_test(
        NAME mini_malloc_hole_capacity_test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/hole_live_capacity_test.py
            $<TARGET_FILE:mini_malloc_hole_test_hook>
            $<TARGET_FILE:mini_hole_live_capacity_test>
    )
    add_test(
        NAME mini_native_replay_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/native_replay_test.py
    )
    add_test(
        NAME mini_synthetic_mstress_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/synthetic_mstress_test.py
    )
endif()
