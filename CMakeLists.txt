project(rime-copilot)
cmake_minimum_required(VERSION 3.10)

option(BUILD_TOOLS "Build tools" ON)

# ✨ 加载 LlamaConfig
if(NOT DEFINED LLAMA_DIR)
  set(LLAMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LlamaConfig.cmake)

aux_source_directory(src copilot_src)

add_library(rime-copilot-objs OBJECT ${copilot_src})
target_link_libraries(rime-copilot-objs PUBLIC llama)
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-copilot-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-copilot PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-copilot-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} llama PARENT_SCOPE)
set(plugin_modules "copilot" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()
