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

# The MLX scoring backend is Apple-only and needs C++20, while librime's root
# sets CMAKE_CXX_STANDARD 17 for everything. Both are handled by keeping it out
# of the default object library and giving it a target of its own below --
# REMOVE_ITEM unconditionally, the same way the ObjC++ sources are, so a Linux
# build never sees the file rather than relying on a preprocessor guard inside
# it.
list(REMOVE_ITEM copilot_src src/mlx_scorer.cc)

set(LINK_LIBS llama nlohmann_json::nlohmann_json)
if (APPLE)
  find_library(IOKIT_LIBRARY IOKit REQUIRED)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  list(APPEND LINK_LIBS ${IOKIT_LIBRARY} ${COREFOUNDATION_LIBRARY} ${APPKIT_LIBRARY})
  # ObjC++ source for IMK client access and frontmost-app queries (not picked
  # up by aux_source_directory)
  list(APPEND copilot_src src/imk_client.mm src/frontmost_app.mm)

  # MLX is opt-in at configure time, not a new hard dependency: it is ~200 MB
  # of runtime artifacts (libmlx 21 MB, libjaccl 1.5 MB, mlx.metallib 174 MB)
  # against a 5.8 MB plugin, and nothing needs it unless
  # copilot/rerank/llm/backend is set to mlx. Point MLX_ROOT at a pip-installed
  # copy:
  #
  #   python3 -m venv /path/to/mlx-venv && /path/to/mlx-venv/bin/pip install mlx
  #   cmake -B build -DCOPILOT_WITH_MLX=ON \
  #         -DMLX_ROOT=/path/to/mlx-venv/lib/python3.12/site-packages/mlx
  #
  option(COPILOT_WITH_MLX "Build the MLX scoring backend (Apple only)" OFF)
  if(COPILOT_WITH_MLX)
    if(NOT MLX_ROOT)
      message(FATAL_ERROR "COPILOT_WITH_MLX is ON but MLX_ROOT is not set")
    endif()
    add_library(rime-copilot-mlx OBJECT src/mlx_scorer.cc)
    # C++20 on this target ONLY. MLX's headers require it; the rest of the
    # plugin compiles as librime's C++17 and must keep doing so.
    target_compile_features(rime-copilot-mlx PRIVATE cxx_std_20)
    target_compile_definitions(rime-copilot-mlx PRIVATE COPILOT_WITH_MLX=1)
    target_include_directories(rime-copilot-mlx PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${MLX_ROOT}/include
      ${MLX_ROOT}/include/metal_cpp)
    target_link_libraries(rime-copilot-mlx PRIVATE llama nlohmann_json::nlohmann_json)
    set_target_properties(rime-copilot-mlx PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND LINK_LIBS ${MLX_ROOT}/lib/libmlx.dylib)
    add_compile_definitions(COPILOT_WITH_MLX=1)
  endif()
endif()

add_library(rime-copilot-objs OBJECT ${copilot_src})
# Every consumer that embeds the plugin objects must embed the MLX ones too,
# because copilot_engine.cc references MlxScorer under COPILOT_WITH_MLX and an
# object library does not carry its dependencies. One variable rather than a
# repeated generator expression: the tools and the test target each embed these
# separately, and a consumer that forgot would fail to link only when the
# option is ON -- i.e. not in CI.
set(COPILOT_ALL_OBJS $<TARGET_OBJECTS:rime-copilot-objs>)
if(TARGET rime-copilot-mlx)
  list(APPEND COPILOT_ALL_OBJS $<TARGET_OBJECTS:rime-copilot-mlx>)
endif()
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
