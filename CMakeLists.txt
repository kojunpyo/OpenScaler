cmake_minimum_required(VERSION 3.20)
project(OpenScaler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FSR 셰이더는 AMD 헤더가 커서 빌드할 때 fxc로 미리 컴파일해 바이트코드 헤더로 만든다.
find_program(FXC fxc REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

set(FSR_DEPS ${CMAKE_SOURCE_DIR}/src/fsr.hlsl
             ${CMAKE_SOURCE_DIR}/third_party/fsr/ffx_a.h
             ${CMAKE_SOURCE_DIR}/third_party/fsr/ffx_fsr1.h)

function(compile_ps entry)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${entry}.h
        COMMAND ${FXC} /nologo /T ps_5_0 /O3 /E ${entry} /Vn g_${entry}
                /I ${CMAKE_SOURCE_DIR}/third_party/fsr
                /Fh ${SHADER_OUT}/${entry}.h ${CMAKE_SOURCE_DIR}/src/fsr.hlsl
        DEPENDS ${FSR_DEPS}
        COMMENT "Compiling ${entry}")
endfunction()
compile_ps(PSEasu)
compile_ps(PSRcas)

add_executable(OpenScaler
    src/main.cpp
    src/shaders.h
    src/fsr_constants.cpp
    src/fsr_constants.h
    ${SHADER_OUT}/PSEasu.h
    ${SHADER_OUT}/PSRcas.h)

target_include_directories(OpenScaler PRIVATE ${SHADER_OUT} third_party/fsr)
target_compile_definitions(OpenScaler PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(OpenScaler PRIVATE /utf-8 /W3 /EHsc /permissive-)
target_link_libraries(OpenScaler PRIVATE d3d11 dxgi d3dcompiler d2d1 dwrite dwmapi magnification windowsapp)
