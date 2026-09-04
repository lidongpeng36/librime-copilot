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

# Declared as `json`, not `nlohmann_json`, and that is not cosmetic: MLX and
# JACCL each FetchContent nlohmann/json under the name `json`, and FetchContent
# dedupes by DECLARED NAME. Under two different names both get populated, both
# CMakeLists run, and the second `add_library(nlohmann_json ...)` fails with
# "another target with the same name already exists". First declaration wins,
# so ours does, and MLX reuses it. The imported target
# `nlohmann_json::nlohmann_json` is unchanged either way.
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)

FetchContent_MakeAvailable(llama json)

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
    # FROM SOURCE, with MLX's own gguf support OFF, and that flag is the whole
    # reason this is not the pip wheel.
    #
    # The wheel's libmlx.dylib EXPORTS 31 gguf_*/ggml_* symbols -- MLX vendors
    # its own ggml to read gguf -- and llama.cpp defines the same 1150. Linking
    # both, the dynamic linker binds llama.cpp's own model loader to MLX's
    # incompatible implementation whenever libmlx precedes the ggml archives on
    # the link line, and llama_model_loader dies with SIGBUS inside
    # gguf_get_key. Which one wins is pure link order: the first version of
    # this backend happened to order them the other way, ran, and produced
    # every measurement in this tree's MLX records. It was never right, only
    # lucky.
    #
    # MLX_BUILD_GGUF=OFF removes the symbols instead of arranging for them to
    # lose, so there is nothing left to get wrong. It costs mx::load_gguf,
    # which MlxScorer used to read the model -- see LoadGgufWeights there, which
    # reads the same file through llama.cpp's gguf API (already linked) and
    # converts ggml Q8_0 into MLX's packed affine form.
    #
    # Needs Apple's Metal toolchain (`xcodebuild -downloadComponent
    # MetalToolchain`, ~700 MB) because the kernels are compiled here rather
    # than shipped prebuilt. In exchange the metallib targets this machine's
    # GPU family instead of every one, and MLX links STATICALLY under this
    # tree's BUILD_SHARED_LIBS OFF -- so there is no libmlx.dylib to deploy or
    # to find at runtime, and no rpath question at all.
    set(MLX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MLX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MLX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(MLX_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    set(MLX_BUILD_GGUF OFF CACHE BOOL "" FORCE)
    set(MLX_BUILD_SAFETENSORS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      mlx
      GIT_REPOSITORY https://github.com/ml-explore/mlx
      GIT_TAG v0.32.1)
    # MLX's CMakeLists sets CMAKE_CXX_STANDARD 20, and a `set()` in a
    # FetchContent subdirectory leaks into THIS directory scope from here on --
    # so every plugin source after this line would compile as C++20. librime's
    # own common.h does not: under C++20 `std::u8string` is char8_t-based and
    # has no operator<<, so the whole plugin stopped building with an error
    # pointing at librime rather than at anything here. Save and restore around
    # the call; rime-copilot-mlx asks for cxx_std_20 on its own target, which is
    # the only place that needs it.
    # MLX as a SHARED library, alone in this tree.
    #
    # Everything else here is static (BUILD_SHARED_LIBS OFF at the top of this
    # file), and MLX static does not work: it registers its Metal backend from
    # a static initializer, so a linker pulling only referenced objects drops
    # the registration and the first GPU operation throws "There is no
    # Stream(gpu, 0) in current thread." $<LINK_LIBRARY:WHOLE_ARCHIVE,mlx> is
    # the textbook answer and did not fix it -- measured: the archive was
    # force-loaded into the executables while librime kept one mlx symbol, so
    # MLX's Scheduler existed in a different image from the code using it.
    #
    # A dylib has no such problem: every object is present, the globals are
    # single, and the process gets one MLX. It costs a file to deploy, which
    # the vault already carries and `rime-copilot status`'s `mlx:` line already
    # checks. MLX_BUILD_GGUF=OFF is what solves the symbol collision, and that
    # is independent of static versus shared.
    set(_copilot_saved_cxx_standard ${CMAKE_CXX_STANDARD})
    set(_copilot_saved_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS ON)
    FetchContent_MakeAvailable(mlx)
    set(BUILD_SHARED_LIBS ${_copilot_saved_shared})
    set(CMAKE_CXX_STANDARD ${_copilot_saved_cxx_standard})
    # And drop mlx's INTERFACE compile feature. It requests cxx_std_20, which
    # CMake propagates to EVERY target that links mlx -- rime-copilot-objs, the
    # tools, the test binary, and librime itself, all of which then compile as
    # C++20 where librime's own common.h fails (std::u8string is char8_t-based
    # there and has no operator<<). $<LINK_ONLY:mlx> does not help: it
    # suppresses link-interface propagation, not compile features -- measured.
    #
    # Safe because it is a requirement on CONSUMERS, not on mlx: mlx is already
    # compiled by the time this runs, and the one consumer whose source needs
    # C++20 to parse MLX's headers, rime-copilot-mlx, asks for cxx_std_20 on
    # its own target.
    set_target_properties(mlx PROPERTIES INTERFACE_COMPILE_FEATURES "")

    add_library(rime-copilot-mlx OBJECT src/mlx_scorer.cc)
    # C++20 on this target ONLY. MLX's headers require it; the rest of the
    # plugin compiles as librime's C++17 and must keep doing so.
    target_compile_features(rime-copilot-mlx PRIVATE cxx_std_20)
    target_compile_definitions(rime-copilot-mlx PRIVATE COPILOT_WITH_MLX=1)
    target_include_directories(rime-copilot-mlx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(rime-copilot-mlx PRIVATE mlx llama nlohmann_json::nlohmann_json)
    set_target_properties(rime-copilot-mlx PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # $<LINK_ONLY:>, not a bare `mlx`. The mlx target carries cxx_std_20 as an
    # INTERFACE compile feature, and LINK_LIBS flows into rime-copilot-objs's
    # PUBLIC link libraries -- so a bare entry compiles EVERY plugin source as
    # C++20, where librime's own common.h stops building (std::u8string becomes
    # char8_t-based and loses operator<<). LINK_ONLY takes the library for
    # linking and none of its usage requirements, which is exactly the
    # distinction: only src/mlx_scorer.cc needs C++20, and it asks for it on
    # its own target.
    # NOT in LINK_LIBS, and $<LINK_ONLY:mlx> there does not work either --
    # measured: the mlx target carries cxx_std_20 as an INTERFACE COMPILE
    # FEATURE, LINK_ONLY suppresses only link-interface propagation, and the
    # feature still reached rime-copilot-objs and compiled every plugin source
    # as C++20. librime's own common.h does not build there (std::u8string
    # becomes char8_t-based and loses operator<<), so the failure pointed at
    # librime rather than at anything here.
    #
    # A separate variable, consumed only where a real binary is linked. The
    # object libraries never mention mlx, so nothing inherits the feature; the
    # one translation unit that needs C++20 asks for it on its own target.
    # WHOLE_ARCHIVE, because MLX registers its Metal backend from a static
    # initializer. Under BUILD_SHARED_LIBS OFF mlx is a static archive, and a
    # linker pulls from an archive only the objects that resolve a referenced
    # symbol -- nothing references the registration, so it is dropped and the
    # first GPU operation throws "There is no Stream(gpu, 0) in current
    # thread." The wheel had no such problem because it is a dylib, where every
    # object is present whether referenced or not.
    # ONE binary in the process gets MLX, and that is librime itself (below,
    # through plugin_deps). The tools and the test embed COPILOT_ALL_OBJS --
    # mlx_scorer.cc.o among them -- and also link librime, so their MLX symbols
    # resolve against the copy already in that dylib.
    #
    # Giving them their own copy as well is not merely wasteful: MLX keeps
    # global state (its Scheduler, which owns the per-device streams), and two
    # copies in one process means the stream created by one is invisible to the
    # other. It fails as "There is no Stream(gpu, 0) in current thread." at the
    # first GPU operation -- measured, with 26398 mlx symbols in the executable
    # beside the dylib's own.
    # LINK_ONLY around the WHOLE_ARCHIVE: librime links plugin_deps PUBLICly,
    # so without it every consumer of librime -- each tool, the test binary --
    # inherits `-force_load libmlx.a` and ends up with its own copy of MLX's
    # globals beside the dylib's. Here LINK_ONLY is the right tool and does
    # work, unlike the earlier attempt to use it against a compile FEATURE.
    # A plain target now: mlx is a dylib, so linking it puts its symbols in
    # libmlx.dylib and every consumer shares that one copy. No WHOLE_ARCHIVE,
    # no LINK_ONLY -- both existed to work around static linking.
    #
    # @loader_path first in the rpath, so a deployed plugin finds libmlx.dylib
    # beside itself rather than in whatever build tree produced it. The build
    # tree's own path follows, for running out of build2 without copying.
    set(COPILOT_MLX_LINK mlx)
    list(APPEND LINK_LIBS "-Wl,-rpath,@loader_path")
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
set(plugin_deps ${rime_library} ${LINK_LIBS} ${COPILOT_MLX_LINK} PARENT_SCOPE)
set(plugin_modules "copilot" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# Unit tests. BUILD_TEST is a librime root option (default ON); GTest is found
# there before plugins are configured.
if(BUILD_TEST AND GTEST_FOUND)
  add_subdirectory(test)
endif()
