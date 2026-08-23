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

# Deliberately compiles nothing from ../src and links no rime library, no
# glog: score_candidates.cc reaches src/scoring_form.h (context alignment and
# tokenization), which reaches src/utf8_index.h (UTF-8 indexing) -- both are
# header-only with no matching `.cc`, precisely so this target never needs
# more than llama.cpp + nlohmann_json. Adding a `.cc` from ../src here (once
# tried for history.cc's copilot::UTF8, before it was extracted into
# utf8_index.h) would silently make this target's link depend on
# CMAKE_BUILD_TYPE: history.cc's DLOG(INFO) calls compile to real,
# glog-linking LOG() whenever NDEBUG is undefined, which a plain
# `cmake -B build` (no build type) leaves undefined -- see the 2026-08-23
# surrounding-context design.
add_executable(score_candidates
  score_candidates.cc)
target_link_libraries(score_candidates
  ${LINK_LIBS})

# Same linkage as score_candidates: llama.cpp and nlohmann_json only, no rime,
# no glog. It measures the batched scoring path by replicating it, not by
# calling into the plugin.
add_executable(bench_scorer
  bench_scorer.cc)
target_link_libraries(bench_scorer
  ${LINK_LIBS})

if(APPLE)
  find_library(APPSERVICES_LIBRARY ApplicationServices REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  add_executable(ax_poc ax_poc.mm)
  target_link_libraries(ax_poc ${APPSERVICES_LIBRARY} ${APPKIT_LIBRARY})
  set_target_properties(ax_poc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
