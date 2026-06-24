cmake_minimum_required(VERSION 3.10)
project(mini_malloc_free_hook C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(mini_malloc_free_hook SHARED malloc_free_hook.c)
target_compile_options(mini_malloc_free_hook PRIVATE
    -fPIC
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(mini_malloc_free_hook PRIVATE dl pthread)
target_link_options(mini_malloc_free_hook PRIVATE "-Wl,--no-undefined")

add_executable(mini_hook_smoke_test smoke_test.c)

enable_testing()
add_test(
    NAME mini_hook_smoke_test
    COMMAND
        ${CMAKE_COMMAND} -E env
        "MINI_HOOK_OUTPUT_DIR=/tmp"
        "LD_PRELOAD=$<TARGET_FILE:mini_malloc_free_hook>"
        $<TARGET_FILE:mini_hook_smoke_test>
)
