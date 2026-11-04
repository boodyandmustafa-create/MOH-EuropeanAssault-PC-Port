cmake_minimum_required(VERSION 3.20)
project(kernel_file_relative_test C)
# Cross-platform: xbox_NtCreateFile and friends called directly, so it covers
# the Win32 backend on Windows/Proton and the POSIX one elsewhere.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_file_relative_test test_main.c)
target_include_directories(kernel_file_relative_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_file_relative_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_file_relative_test COMMAND kernel_file_relative_test)
