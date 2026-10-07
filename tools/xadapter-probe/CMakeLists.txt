cmake_minimum_required(VERSION 3.24)
project(xadapter_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(xadapter-probe main.cpp)
target_compile_definitions(xadapter-probe PRIVATE UNICODE _UNICODE)
target_link_libraries(xadapter-probe PRIVATE d3d12 dxgi)
if(MSVC)
  target_compile_options(xadapter-probe PRIVATE /W4 /permissive- /utf-8)
endif()
