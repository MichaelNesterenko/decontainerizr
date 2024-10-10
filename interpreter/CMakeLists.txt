cmake_minimum_required(VERSION 3.22.1)
project(interpreter CXX)

add_executable(interpreter intp.cpp)
target_link_options(interpreter PUBLIC -static)
