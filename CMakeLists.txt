cmake_minimum_required(VERSION 3.20)

project(audio_assist_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(audio_assist_overlay WIN32
    src/main.cpp
    src/overlay/DirectionalEventParser.cpp
    src/overlay/OverlayService.cpp
)

target_include_directories(audio_assist_overlay PRIVATE src)

target_compile_definitions(audio_assist_overlay PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(audio_assist_overlay PRIVATE
    d2d1
    dwrite
    user32
    gdi32
    shell32
)