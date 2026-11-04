cmake_minimum_required(VERSION 3.20)
project(mmio_decode_a64_test C)
set(CMAKE_C_STANDARD 11)
# Header-only decoder: no runtime to build, just the include path. The test
# faults for real (PROT_NONE page + sigaction), so it needs a POSIX arm64
# host; elsewhere it exits 77 (skipped).
add_executable(mmio_decode_a64_test test_main.c)
target_include_directories(mmio_decode_a64_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME mmio_decode_a64 COMMAND mmio_decode_a64_test)
set_tests_properties(mmio_decode_a64 PROPERTIES SKIP_RETURN_CODE 77)
