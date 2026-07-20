# CMake generated Testfile for 
# Source directory: /home/whq/Desktop/code_list/test_profiler/mini
# Build directory: /home/whq/Desktop/code_list/test_profiler/mini/build-rply
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(mini_hook_smoke_test "/usr/local/bin/cmake" "-E" "env" "LD_PRELOAD=/home/whq/Desktop/code_list/test_profiler/mini/build-rply/libmini_malloc_free_hook.so" "/home/whq/Desktop/code_list/test_profiler/mini/build-rply/mini_hook_smoke_test")
set_tests_properties(mini_hook_smoke_test PROPERTIES  _BACKTRACE_TRIPLES "/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;83;add_test;/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;0;")
add_test(mini_native_replay_test "/usr/bin/python3.10" "/home/whq/Desktop/code_list/test_profiler/mini/native_replay_test.py")
set_tests_properties(mini_native_replay_test PROPERTIES  _BACKTRACE_TRIPLES "/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;93;add_test;/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;0;")
add_test(mini_synthetic_mstress_test "/usr/bin/python3.10" "/home/whq/Desktop/code_list/test_profiler/mini/synthetic_mstress_test.py")
set_tests_properties(mini_synthetic_mstress_test PROPERTIES  _BACKTRACE_TRIPLES "/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;97;add_test;/home/whq/Desktop/code_list/test_profiler/mini/CMakeLists.txt;0;")
