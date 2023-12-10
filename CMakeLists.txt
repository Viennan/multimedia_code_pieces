cmake_minimum_required(VERSION 3.22)

project(MultiMediaCodePieces)

set(CMAKE_CXX_STANDARD 17)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindFFmpeg.cmake)

add_subdirectory(examples)