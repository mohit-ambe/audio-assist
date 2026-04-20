cmake_minimum_required(VERSION 3.20)

project(audio_assist LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(audio_assist
    src/main.cpp
    src/audio/AudioCaptureService.cpp
    src/audio/FeaturePipeline.cpp
)

target_include_directories(audio_assist PRIVATE src)

target_compile_definitions(audio_assist PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(audio_assist PRIVATE
    avrt
    ole32
    uuid
)
