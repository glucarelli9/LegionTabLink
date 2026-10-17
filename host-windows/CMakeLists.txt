cmake_minimum_required(VERSION 3.21)
project(LegionTabLinkHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(host-streamer
    src/main.cpp
    src/DxgiCapture.cpp
    src/EncoderFactory.cpp
    src/AmfEncoder.cpp
    src/NvencEncoder.cpp
    src/AdbSocketServer.cpp
    src/PenTouchInjector.cpp
)

target_include_directories(host-streamer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_compile_definitions(host-streamer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(host-streamer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wno-unused-parameter>
)

target_link_libraries(host-streamer PRIVATE
    ws2_32
    d3d11
    dxgi
    ole32
    user32
    gdi32
)

set_property(TARGET host-streamer PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
