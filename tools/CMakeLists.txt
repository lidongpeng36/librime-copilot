set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

aux_source_directory(. build_copilot_src)
include_directories(../src)

add_executable(build_copilot
  ${build_copilot_src}
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(build_copilot
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library})
