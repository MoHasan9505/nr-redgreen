cmake_minimum_required(VERSION 3.24)
project(nvapi_gate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Built as nvapi64.dll so the game's LoadLibrary("nvapi64.dll") finds it beside the exe.
add_library(nvapi-gate SHARED main.cpp nvapi64.def)
set_target_properties(nvapi-gate PROPERTIES OUTPUT_NAME "nvapi64")
target_compile_definitions(nvapi-gate PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(nvapi-gate PRIVATE /W4 /permissive- /utf-8)
  # Static CRT: nothing extra to deploy next to the game.
  set_property(TARGET nvapi-gate PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
