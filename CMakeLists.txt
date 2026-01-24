project(rime-copilot)
cmake_minimum_required(VERSION 3.10)

option(BUILD_TOOLS "Build tools" ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(FetchContent)
FetchContent_Declare(
  llama
  GIT_REPOSITORY https://github.com/ggml-org/llama.cpp
  GIT_TAG b7820)
FetchContent_MakeAvailable(llama)

aux_source_directory(src copilot_src)

set(LINK_LIBS llama)
if (APPLE)
  find_library(IOKIT_LIBRARY IOKit REQUIRED)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
  list(APPEND LINK_LIBS ${IOKIT_LIBRARY} ${COREFOUNDATION_LIBRARY})
endif()

add_library(rime-copilot-objs OBJECT ${copilot_src})
target_link_libraries(rime-copilot-objs PUBLIC ${LINK_LIBS})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-copilot-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-copilot PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-copilot-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} ${LINK_LIBS} PARENT_SCOPE)
set(plugin_modules "copilot" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()
