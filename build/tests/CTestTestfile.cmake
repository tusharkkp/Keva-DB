# CMake generated Testfile for 
# Source directory: /mnt/c/PROJECTS/Keva/tests
# Build directory: /mnt/c/PROJECTS/Keva/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[keva_test_dict]=] "/mnt/c/PROJECTS/Keva/build/tests/keva_test_dict")
set_tests_properties([=[keva_test_dict]=] PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;45;add_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;51;add_keva_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;0;")
add_test([=[keva_test_buffer]=] "/mnt/c/PROJECTS/Keva/build/tests/keva_test_buffer")
set_tests_properties([=[keva_test_buffer]=] PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;45;add_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;52;add_keva_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;0;")
add_test([=[keva_test_resp]=] "/mnt/c/PROJECTS/Keva/build/tests/keva_test_resp")
set_tests_properties([=[keva_test_resp]=] PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;45;add_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;53;add_keva_test;/mnt/c/PROJECTS/Keva/tests/CMakeLists.txt;0;")
subdirs("../_deps/catch2-build")
