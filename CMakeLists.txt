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
  GIT_TAG b10456)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)

FetchContent_MakeAvailable(llama nlohmann_json)

# GCC 13 may emit false-positive -Wmaybe-uninitialized warnings in
# upstream llama.cpp regex code (treated as errors in some CI setups).
# Limit suppression to the third-party llama target only.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(llama PRIVATE
    -Wno-maybe-uninitialized
    -Wno-error=maybe-uninitialized)
endif()

aux_source_directory(src copilot_src)

# `aux_source_directory` may collect Objective-C++ source on non-Apple hosts.
# Keep IMK/AppKit integration strictly macOS-only to avoid Linux CI trying to
# compile `.mm` sources without an Objective-C++ toolchain (cc1objplus).
list(REMOVE_ITEM copilot_src src/imk_client.mm src/frontmost_app.mm)

set(LINK_LIBS llama nlohmann_json::nlohmann_json)
if (APPLE)
  find_library(IOKIT_LIBRARY IOKit REQUIRED)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  list(APPEND LINK_LIBS ${IOKIT_LIBRARY} ${COREFOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
  # ObjC++ source for IMK client access and frontmost-app queries (not picked
  # up by aux_source_directory)
  list(APPEND copilot_src src/imk_client.mm src/frontmost_app.mm)
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

# Unit tests. BUILD_TEST is a librime root option (default ON); GTest is found
# there before plugins are configured.
if(BUILD_TEST AND GTEST_FOUND)
  add_subdirectory(test)
endif()
