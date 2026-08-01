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
  prediction_context_test.cc
  spacing_matrix_test.cc)

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
    ${GTEST_LIBRARIES})
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
