cmake_minimum_required(VERSION 3.25)
project(recover_mp4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)

add_executable(recover_mp4
    src/main.cpp
    src/reference.cpp
    src/scanner.cpp
    src/atoms.cpp
    src/writer.cpp
)

target_link_libraries(recover_mp4 PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
)

target_compile_options(recover_mp4 PRIVATE
    -Wall -Wextra -Wpedantic -O2
)
