set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR})

include_directories(../src)

set(copilot_test_src
  test_main.cc
  auto_spacer_util_test.cc
  candidate_order_test.cc
  commit_text_test.cc
  history_test.cc
  ime_bridge_socket_test.cc
  ime_bridge_state_test.cc
  learning_commit_test.cc
  prediction_context_test.cc
  raw_input_order_test.cc
  replay_align_test.cc
  rerank_llm_test.cc
  rerank_test.cc
  rerank_trace_test.cc
  scoring_form_test.cc
  spacing_matrix_test.cc
  surrounding_source_test.cc
  telemetry_commit_test.cc
  telemetry_event_test.cc
  telemetry_stats_test.cc
  telemetry_test.cc
  tmux_source_test.cc
  tmux_source_util_test.cc
  warm_cache_test.cc)

# GTest include dirs are added globally by the librime root CMakeLists.
#
# Linking strategy depends on how the plugin is built into Rime:
#
# * Merged mode (BUILD_MERGED_PLUGINS=ON, the librime default / CI): the plugin
#   objects and their static deps (llama.cpp) are compiled *into* the Rime
#   library. Re-embedding $<TARGET_OBJECTS:rime-copilot-objs> here would place a
#   second copy of llama's file-scope globals (e.g. unicode-data.cpp's
#   std::unordered_set) in the test executable. With default ELF visibility the
#   duplicate definitions are interposed to a single address but each module
#   still registers its own __cxa_atexit destructor, so the global is destroyed
#   twice at exit -> ASan heap-use-after-free. So we resolve every plugin/llama
#   symbol from the Rime library instead of re-linking them.
#
# * Separate/external mode (BUILD_SEPARATE_LIBS / ENABLE_EXTERNAL_PLUGINS): the
#   Rime library does NOT contain the plugin, so we must embed the plugin
#   objects (and link llama) directly. There is only one copy of llama here, so
#   no double-free.
if(BUILD_MERGED_PLUGINS)
  add_executable(copilot_test ${copilot_test_src})
  target_link_libraries(copilot_test
    ${rime_library}
    ${rime_dict_library}
    ${GTEST_LIBRARIES}
    # Header-only INTERFACE target: contributes include directories only, no
    # object code, so it cannot reintroduce the duplicate-globals problem the
    # comment above describes. Needed because telemetry_event.h includes
    # nlohmann/json.hpp and this branch does not otherwise see its includes.
    nlohmann_json::nlohmann_json)
else()
  add_executable(copilot_test
    ${copilot_test_src}
    $<TARGET_OBJECTS:rime-copilot-objs>)
  target_link_libraries(copilot_test
    ${LINK_LIBS}
    ${rime_library}
    ${rime_dict_library}
    ${GTEST_LIBRARIES})
endif()

# macOS-only ObjC++ test driving the IMK query/compose-detection logic against
# a mock NSTextInputClient.
if(APPLE)
  target_sources(copilot_test PRIVATE imk_client_test.mm)
  find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
  target_link_libraries(copilot_test ${FOUNDATION_LIBRARY})
endif()

add_test(NAME copilot_test COMMAND copilot_test)

# The scoring-form golden fixture is read at run time. Passing the SOURCE path
# rather than copying the file keeps exactly one copy: a stale copy in the build
# tree is precisely the drift this fixture exists to prevent.
target_compile_definitions(copilot_test PRIVATE
  COPILOT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
