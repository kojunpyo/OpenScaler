cmake_minimum_required(VERSION 3.20)
project(OpenScaler VERSION 0.4.0 LANGUAGES CXX RC)

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

# 설정 창 UI: Dear ImGui (MIT, 수정 없음)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE /utf-8)

# WIN32: 콘솔 창 없이 실행되는 프로그램 (진입점은 그대로 wmain)
add_executable(OpenScaler WIN32
    src/main.cpp
    src/app.h
    src/gui.cpp
    src/gui.h
    src/shaders.h
    src/fsr_constants.cpp
    src/fsr_constants.h
    src/resource.h
    src/openscaler.rc
    ${SHADER_OUT}/PSEasu.h
    ${SHADER_OUT}/PSRcas.h)

# 아이콘(assets)과 픽셀 글꼴(Galmuri, SIL OFL 1.1)은 리소스로 실행 파일 안에 넣는다.
target_include_directories(OpenScaler PRIVATE ${SHADER_OUT} third_party/fsr assets third_party/galmuri)
set_source_files_properties(src/openscaler.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/openscaler.ico;${CMAKE_SOURCE_DIR}/third_party/galmuri/Galmuri11-Bold.ttf;${CMAKE_SOURCE_DIR}/third_party/galmuri/Galmuri11-Condensed.ttf")
target_compile_definitions(OpenScaler PRIVATE UNICODE _UNICODE NOMINMAX OPENSCALER_VERSION="${PROJECT_VERSION}")
target_compile_options(OpenScaler PRIVATE /utf-8 /W3 /EHsc /permissive-)
target_link_options(OpenScaler PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(OpenScaler PRIVATE imgui d3d11 dxgi d3dcompiler d2d1 dwrite dwmapi magnification windowsapp)
