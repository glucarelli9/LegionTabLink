cmake_minimum_required(VERSION 3.21)
project(GLExtraScreenHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(GLExtraScreen-Server
    src/main.cpp
    src/DxgiCapture.cpp
    src/EncoderFactory.cpp
    src/AmfEncoder.cpp
    src/NvencEncoder.cpp
    src/AdbSocketServer.cpp
    src/PenTouchInjector.cpp
    src/app.rc
)

target_include_directories(GLExtraScreen-Server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_compile_definitions(GLExtraScreen-Server PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(GLExtraScreen-Server PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wno-unused-parameter>
)

target_link_libraries(GLExtraScreen-Server PRIVATE
    ws2_32
    d3d11
    dxgi
    ole32
    user32
    gdi32
    shell32
)

set_property(TARGET GLExtraScreen-Server PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
