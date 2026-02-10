set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

include_directories(../src)

add_executable(build_copilot
  build_copilot.cc
  $<TARGET_OBJECTS:rime-copilot-objs>)
target_link_libraries(build_copilot
  ${LINK_LIBS}
  ${rime_library}
  ${rime_dict_library})

if(APPLE)
  find_library(APPSERVICES_LIBRARY ApplicationServices REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  add_executable(ax_poc ax_poc.mm)
  target_link_libraries(ax_poc ${APPSERVICES_LIBRARY} ${APPKIT_LIBRARY})
  set_target_properties(ax_poc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
