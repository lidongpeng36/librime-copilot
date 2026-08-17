set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

include_directories(../src)

add_executable(build_copilot
  build_copilot.cc
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(build_copilot
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library})

add_executable(dump_copilot
  dump_copilot.cc
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(dump_copilot
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library})

add_executable(replay_copilot
  replay_copilot.cc
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(replay_copilot
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library})

add_executable(score_candidates
  score_candidates.cc)
target_link_libraries(score_candidates
  ${LINK_LIBS})

if(APPLE)
  find_library(APPSERVICES_LIBRARY ApplicationServices REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  add_executable(ax_poc ax_poc.mm)
  target_link_libraries(ax_poc ${APPSERVICES_LIBRARY} ${APPKIT_LIBRARY})
  set_target_properties(ax_poc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
