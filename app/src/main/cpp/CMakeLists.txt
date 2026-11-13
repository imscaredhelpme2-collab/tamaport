cmake_minimum_required(VERSION 3.22.1)
project(tgemu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Unicorn CPU emulator (ARM guest only). Uses third_party/unicorn if present, otherwise downloads it. ----
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(UNICORN_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicorn")
if(EXISTS "${UNICORN_LOCAL}/CMakeLists.txt")
    add_subdirectory(${UNICORN_LOCAL} unicorn-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(unicorn
        GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
        GIT_TAG 2.1.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(unicorn)
endif()

add_library(tgemu SHARED
    jni_bridge.cpp
    engine/engine.cpp
    engine/engine_jni.cpp
    engine/gles1.cpp)

target_include_directories(tgemu PRIVATE engine)
target_compile_options(tgemu PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
# 16 KB page-size compatible on Android 15+ devices
target_link_options(tgemu PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(tgemu PRIVATE unicorn GLESv2 log)
