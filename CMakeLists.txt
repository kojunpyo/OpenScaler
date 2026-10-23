cmake_minimum_required(VERSION 3.20)
project(OpenScaler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(OpenScaler src/main.cpp src/shaders.h)

target_compile_definitions(OpenScaler PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(OpenScaler PRIVATE /utf-8 /W3 /EHsc /permissive-)
target_link_libraries(OpenScaler PRIVATE d3d11 dxgi d3dcompiler dwmapi magnification windowsapp)
