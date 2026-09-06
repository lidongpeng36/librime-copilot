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

# Declared as `json`, not `nlohmann_json`. The reason was the MLX backend --
# MLX and JACCL each FetchContent nlohmann/json under the name `json`, and
# FetchContent dedupes by DECLARED NAME, so two names meant two populations and
# a duplicate `add_library(nlohmann_json ...)`. That backend is gone (see
# CLAUDE.md, "The MLX backend, and why it is not here"), and the name is kept
# rather than reverted: it costs nothing, the imported target
# `nlohmann_json::nlohmann_json` is unchanged either way, and any future
# dependency that vendors nlohmann/json will collide the same way.
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)

# Keep a system ggml.h/llama.h from shadowing the vendored one.
#
# librime's root CMakeLists calls include_directories() for Boost, glog,
# yaml-cpp, leveldb, marisa and opencc; on macOS every one of those resolves to
# /opt/homebrew/include, which is then inherited by this directory and lands
# FIRST on every compile line below -- ahead of the fetched ggml's own
# ../include. Homebrew's `ggml` and `llama.cpp` formulae install ggml.h and
# llama.h into exactly that prefix, so with either installed, ggml.c compiles
# against a DIFFERENT ggml.h than the tree it belongs to and dies on
#
#   ggml.c:1258: static assertion failed due to requirement
#   'GGML_GLU_OP_COUNT == 6': GGML_GLU_OP_COUNT != 6
#
# which names neither Homebrew nor the include path. Measured 2026-09-05: a
# clean configure that had worked for weeks broke the moment `brew install
# llama.cpp` pulled in ggml 0.23.0. Nothing in this repo changed.
#
# The general shape is Homebrew's /opt/homebrew/include supplying headers for a
# library fetched elsewhere. Fixed by taking the prefix out of the include list
# for the vendored subtree ONLY, and saying so out loud. Our own targets keep
# it -- they need Boost and glog from there.
get_property(_copilot_dir_includes DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
set(_copilot_shadowing "")
foreach(_dir IN LISTS _copilot_dir_includes)
  if(EXISTS "${_dir}/ggml.h" OR EXISTS "${_dir}/llama.h")
    list(APPEND _copilot_shadowing "${_dir}")
  endif()
endforeach()
if(_copilot_shadowing)
  message(WARNING
    "copilot: ${_copilot_shadowing} carries its own ggml.h/llama.h and would "
    "shadow the llama.cpp fetched here. Dropping it from the include path for "
    "the vendored subtree. This is a system package (brew: ggml, llama.cpp) "
    "that nothing in this build needs -- uninstalling it is the tidier fix.")
  set(_copilot_kept_includes ${_copilot_dir_includes})
  list(REMOVE_ITEM _copilot_kept_includes ${_copilot_shadowing})
  set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES ${_copilot_kept_includes})
endif()

FetchContent_MakeAvailable(llama json)

if(_copilot_shadowing)
  set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES ${_copilot_dir_includes})
endif()

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
# One variable rather than the generator expression repeated at each of the
# four places that embed the plugin objects (the tools, the test target). It
# held a second object library while the MLX backend existed; kept as the one
# name those consumers spell so adding another object library stays a one-line
# change here rather than an edit in three CMakeLists.
set(COPILOT_ALL_OBJS $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(rime-copilot-objs PUBLIC ${LINK_LIBS})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-copilot-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-copilot PARENT_SCOPE)
set(plugin_objs ${COPILOT_ALL_OBJS} PARENT_SCOPE)
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
