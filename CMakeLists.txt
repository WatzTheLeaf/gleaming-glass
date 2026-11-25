cmake_minimum_required(VERSION 3.20)
project(gleaming-glass LANGUAGES CXX)

if (NOT MINGW)
    message(FATAL_ERROR "Please compile using MinGW-w64.")
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

add_executable(gleaming-glass WIN32
        src/common.cpp
        src/main.cpp
        src/renderer.cpp
        src/win32.cpp
        src/desktop.cpp
        src/video.cpp
)

target_compile_features(gleaming-glass PRIVATE cxx_std_17)
target_compile_definitions(gleaming-glass PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
)
target_compile_options(gleaming-glass PRIVATE -Wall -Wextra -mcrtdll=ucrt)

target_link_options(gleaming-glass PRIVATE
        -mcrtdll=ucrt -municode -static $<$<CONFIG:Release>:-s>
)

target_link_libraries(gleaming-glass PRIVATE
        d3d11 dxgi d3dcompiler
        mfplat mfreadwrite mfuuid
        ole32 shell32 user32 gdi32
)