cmake_minimum_required(VERSION 3.20)
project(tf3_framegen LANGUAGES CXX ASM_MASM RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no dependency on the game's redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root")

# Compute shaders -> SPIR-V embedded as C arrays (kMvgenSpv, ...).
set(GLSLANG "${VULKAN_SDK_DIR}/Bin/glslangValidator.exe")
set(SHADER_HEADERS)
foreach(name mvgen errstat mvvis)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${name}.comp")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.h")
    string(SUBSTRING ${name} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${name} 1 -1 rest)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${GLSLANG} -V --target-env vulkan1.1 --vn k${first}${rest}Spv -o ${out} ${src}
        DEPENDS ${src} "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/common.glsl"
        VERBATIM)
    list(APPEND SHADER_HEADERS ${out})
endforeach()

add_library(winhttp SHARED
    src/main.cpp
    src/vk_hooks.cpp
    src/framegen.cpp
    src/streamline.cpp
    src/version.rc
    src/proxy/winhttp_stubs.asm
    src/proxy/winhttp.def
    ${SHADER_HEADERS}
)
set(STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/streamline")
target_include_directories(winhttp PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}" "${VULKAN_SDK_DIR}/Include"
    "${STREAMLINE_DIR}/include")

# NVIDIA's production Streamline binaries, loaded at runtime from <exe dir>/framegen.
set(STREAMLINE_RUNTIME sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll
    NvLowLatencyVk.dll nvngx_dlss.license.txt reflex.license.txt)
foreach(f ${STREAMLINE_RUNTIME})
    add_custom_command(TARGET winhttp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STREAMLINE_DIR}/bin/x64/${f}"
                "${CMAKE_CURRENT_BINARY_DIR}/framegen/${f}")
endforeach()
target_compile_definitions(winhttp PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(winhttp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /permissive- /Zi /utf-8>)
target_link_options(winhttp PRIVATE /DEBUG /OPT:REF /OPT:ICF)

# Loads Vulkan the way the game does, through our winhttp.dll next to it.
add_executable(smoke_test tests/smoke_test.cpp)
target_include_directories(smoke_test PRIVATE "${VULKAN_SDK_DIR}/Include")
target_link_libraries(smoke_test PRIVATE winhttp)

# Renders game-like frames through our DLL under the Khronos validation layer.
add_executable(frame_test tests/frame_test.cpp)
target_include_directories(frame_test PRIVATE "${VULKAN_SDK_DIR}/Include")
target_link_libraries(frame_test PRIVATE winhttp user32)

# Surface created through the game's own SDL2.dll, the way Transport Fever 3 does it.
set(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SDL2.lib"
    COMMAND lib /nologo /def:"${CMAKE_CURRENT_SOURCE_DIR}/tests/sdl2_imports.def"
            /out:"${CMAKE_CURRENT_BINARY_DIR}/SDL2.lib" /machine:x64
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/sdl2_imports.def")
add_custom_target(sdl2_implib DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/SDL2.lib")
add_executable(sdl_test tests/sdl_test.cpp)
add_dependencies(sdl_test sdl2_implib)
target_include_directories(sdl_test PRIVATE "${VULKAN_SDK_DIR}/Include")
target_link_libraries(sdl_test PRIVATE winhttp "${CMAKE_CURRENT_BINARY_DIR}/SDL2.lib")
# The test runs with the game's own SDL2.dll when this folder sits inside the game folder.
if(EXISTS "${GAME_DIR}/SDL2.dll")
    add_custom_command(TARGET sdl_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GAME_DIR}/SDL2.dll" "${CMAKE_CURRENT_BINARY_DIR}/SDL2.dll")
endif()
