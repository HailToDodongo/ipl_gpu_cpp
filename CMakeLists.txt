# Note: Dummy file, just to make CLion happy

cmake_minimum_required(VERSION 3.26)
project(PifChecksum)

set(CMAKE_CXX_STANDARD 23)

include_directories(.)

add_executable(ipl_gpu main.cpp)
