cmake_minimum_required(VERSION 3.24)
project(nr_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NGX_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ngx")
if(NOT EXISTS "${NGX_INCLUDE}/nvsdk_ngx.h")
  message(FATAL_ERROR "NGX headers missing - run tools\\fetch-deps.ps1 first")
endif()

# NRB6 is shared with the bridge so the fix tested here is the one that ships.
add_executable(nr-probe main.cpp ../../bridge/src/nrb_dxgi_unhide.cpp)
target_include_directories(nr-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../bridge/src")
# The DLSS-NR snippet only serves callers whose module path contains "nvngx.dll".
set_target_properties(nr-probe PROPERTIES OUTPUT_NAME "nvngx.dll_nr-probe")
target_include_directories(nr-probe PRIVATE "${NGX_INCLUDE}")
target_compile_definitions(nr-probe PRIVATE UNICODE _UNICODE)
target_link_libraries(nr-probe PRIVATE d3d12 dxgi dxcore windowscodecs ole32 advapi32 version)
if(MSVC)
  target_compile_options(nr-probe PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
