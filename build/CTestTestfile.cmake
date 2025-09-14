# CMake generated Testfile for 
# Source directory: /home/yugam05/kvstore_project
# Build directory: /home/yugam05/kvstore_project/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(KVStoreTests "/home/yugam05/kvstore_project/build/runTests")
set_tests_properties(KVStoreTests PROPERTIES  _BACKTRACE_TRIPLES "/home/yugam05/kvstore_project/CMakeLists.txt;45;add_test;/home/yugam05/kvstore_project/CMakeLists.txt;0;")
subdirs("googletest")
