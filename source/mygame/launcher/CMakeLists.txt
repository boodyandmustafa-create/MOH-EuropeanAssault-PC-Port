cmake_minimum_required(VERSION 3.20)
project(moh_launcher CXX)

# The launcher is its own small program, built apart from the game project.
set(CMAKE_CXX_STANDARD 17)
# Static runtime: the launcher must start (and say "install the Visual C++ Redistributable") on a PC that lacks it.
# The game program itself keeps the normal dynamic runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_executable(Launcher WIN32 launcher.cpp)
target_compile_definitions(Launcher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(Launcher PRIVATE gdiplus bcrypt shell32 user32 gdi32)
if(MSVC)
    target_compile_options(Launcher PRIVATE /W3 /utf-8)
    target_link_options(Launcher PRIVATE /MANIFEST:NO)
endif()
