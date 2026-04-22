cmake_minimum_required(VERSION 3.20)

project(audio_assist_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(audio_assist_capture
    src/main.cpp
    src/audio/WindowsAudioCaptureService.cpp
    src/audio/CaptureFeaturePipeline.cpp
)

target_include_directories(audio_assist_capture PRIVATE src)

target_compile_definitions(audio_assist_capture PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(audio_assist_capture PRIVATE
    avrt
    ole32
    uuid
)