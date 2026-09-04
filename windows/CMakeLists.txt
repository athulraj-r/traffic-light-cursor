cmake_minimum_required(VERSION 3.16)
project(TrafficLightCursor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_language(RC)

# Windows subsystem — no console window
add_executable(traffic_light WIN32 main.cpp resources.rc)

target_compile_definitions(traffic_light PRIVATE UNICODE _UNICODE)

target_link_libraries(traffic_light
    user32
    gdi32
    comctl32
    shell32
    ole32
    oleaut32
    strmiids
    winmm
    mfplat
    mfplay
    uuid
)

# Optimization and linking flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(traffic_light PRIVATE -O2 -Wall -Wextra)
    target_link_options(traffic_light PRIVATE -static -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_compile_options(traffic_light PRIVATE /O2 /W3)
endif()

