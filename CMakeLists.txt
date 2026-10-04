cmake_minimum_required(VERSION 3.20)
project(StretchScaler LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "StretchScaler requires MSVC (Visual Studio 2022 / Build Tools) and the Windows 10/11 SDK.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT, /MTd for Debug) - must be set before the target is created.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB STRETCHSCALER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# Listing the .manifest as a source makes CMake pass it to the linker (/MANIFESTINPUT, embedded).
add_executable(StretchScaler WIN32
    ${STRETCHSCALER_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc")

set_target_properties(StretchScaler PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/bin")

target_compile_definitions(StretchScaler PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_compile_options(StretchScaler PRIVATE
    /W3 /EHsc /permissive- /Zc:__cplusplus /utf-8)

target_link_options(StretchScaler PRIVATE /MANIFEST:EMBED /MANIFESTUAC:NO)

target_link_libraries(StretchScaler PRIVATE
    d3d11 dxgi d3dcompiler dwmapi windowsapp
    user32 gdi32 comctl32 Magnification ole32)
