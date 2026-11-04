cmake_minimum_required(VERSION 3.20)
project(kernel_fatx_overwrite_test C)
# Cross-platform: the xbox_Nt* file functions called directly, so it covers
# the Win32 backend on Windows/Proton and the POSIX one elsewhere.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_fatx_overwrite_test test_main.c)
target_include_directories(kernel_fatx_overwrite_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_fatx_overwrite_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_fatx_overwrite_test COMMAND kernel_fatx_overwrite_test)
