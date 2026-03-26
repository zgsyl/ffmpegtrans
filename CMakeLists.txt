cmake_minimum_required(VERSION 3.10)
project(FFmpegTranscoder)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Find FFmpeg components dependencies using pkg-config
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

# Create the executable
add_executable(transcoder main.cpp)

# Link against the FFmpeg libraries found via pkg-config
target_link_libraries(transcoder PkgConfig::FFMPEG)
