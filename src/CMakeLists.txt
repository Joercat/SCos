cmake_minimum_required(VERSION 3.12)
project(SCos)

set(CMAKE_CXX_STANDARD 20)

add_executable(scos
    src/kernel/main.cpp
    src/kernel/kernel.cpp
    src/gui/window_manager.cpp
    src/system/file_system.cpp
    src/graphics/display_manager.cpp
    src/apps/terminal.cpp
)

target_link_libraries(scos
    OpenGL
    Vulkan
    X11
)
