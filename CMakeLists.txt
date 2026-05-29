cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MultiTap VERSION 0.1.0 LANGUAGES CXX)

add_executable(MultiTap WIN32
    src/App.cpp
    src/AudioEngine.cpp
    src/DeviceEnumerator.cpp
    src/Config.cpp
    src/MultiTap.rc
)

add_custom_command(TARGET MultiTap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:MultiTap>" "${CMAKE_SOURCE_DIR}/release/MultiTap.exe"
)

target_compile_features(MultiTap PRIVATE cxx_std_17)
target_compile_definitions(MultiTap PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_include_directories(MultiTap PRIVATE src)

target_link_libraries(MultiTap PRIVATE
    ole32
    uuid
    avrt
    comctl32
    dwmapi
    shell32
    shlwapi
    uxtheme
)

if(MSVC)
    target_compile_options(MultiTap PRIVATE /W4 /permissive- /utf-8)
endif()
