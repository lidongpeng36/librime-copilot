set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR})

include_directories(../src)

# GTest include dirs are added globally by the librime root CMakeLists.
add_executable(copilot_test
  test_main.cc
  auto_spacer_util_test.cc
  commit_text_test.cc
  history_test.cc
  spacing_matrix_test.cc
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(copilot_test
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library}
  ${GTEST_LIBRARIES})

# macOS-only ObjC++ test driving the IMK query/compose-detection logic against
# a mock NSTextInputClient.
if(APPLE)
  target_sources(copilot_test PRIVATE imk_client_test.mm)
  find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
  target_link_libraries(copilot_test ${FOUNDATION_LIBRARY})
endif()

add_test(NAME copilot_test COMMAND copilot_test)
