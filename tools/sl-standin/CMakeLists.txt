cmake_minimum_required(VERSION 3.24)
project(sl_standin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Built as sl.interposer.dll: it replaces the game's Streamline interposer (see main.cpp).
# The vk* forwarders to vulkan-1.dll: linker pragmas under MSVC (vk_forwards_msvc.inc), appended
# to the .def for other toolchains.
if(MSVC)
  set(SL_DEF "${CMAKE_CURRENT_SOURCE_DIR}/sl.interposer.def")
else()
  file(READ sl.interposer.def SL_DEF_TEXT)
  file(STRINGS vk_exports.txt VK_NAMES)
  foreach(n IN LISTS VK_NAMES)
    string(APPEND SL_DEF_TEXT "    ${n}=vulkan-1.${n}\n")
  endforeach()
  set(SL_DEF "${CMAKE_CURRENT_BINARY_DIR}/sl.interposer.def")
  file(WRITE "${SL_DEF}" "${SL_DEF_TEXT}")
endif()

add_library(sl-standin SHARED main.cpp "${SL_DEF}")
set_target_properties(sl-standin PROPERTIES OUTPUT_NAME "sl.interposer" PREFIX "")
target_compile_definitions(sl-standin PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(sl-standin PRIVATE /W4 /permissive- /utf-8)
  # Static CRT: nothing extra to deploy next to the game.
  set_property(TARGET sl-standin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_link_options(sl-standin PRIVATE -static -static-libgcc -static-libstdc++)
endif()
