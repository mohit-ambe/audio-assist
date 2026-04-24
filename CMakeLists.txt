cmake_minimum_required(VERSION 3.20)

project(audio_assist LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AUDIO_ASSIST_ENABLE_ONNXRUNTIME "Enable ONNX Runtime backend if the SDK is installed" OFF)

add_executable(audio_assist
    src/main.cpp
    src/audio/AudioCaptureService.cpp
    src/audio/FeaturePipeline.cpp
    src/inference/InferenceService.cpp
    src/overlay/OverlayService.cpp
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
    user32
    gdi32
)

if(MSVC)
    target_compile_options(audio_assist PRIVATE /W4 /permissive-)
else()
    target_compile_options(audio_assist PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(AUDIO_ASSIST_ENABLE_ONNXRUNTIME)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime)

    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "ONNX Runtime SDK not found. Set ONNXRUNTIME_INCLUDE_DIR and ONNXRUNTIME_LIBRARY.")
    endif()

    target_compile_definitions(audio_assist PRIVATE AUDIO_ASSIST_HAS_ONNXRUNTIME=1)
    target_include_directories(audio_assist PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(audio_assist PRIVATE ${ONNXRUNTIME_LIBRARY})
endif()
