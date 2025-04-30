# LlamaConfig.cmake
# 查找 llama + ggml 相关依赖，并定义 llama target

if (NOT TARGET llama)
  # ✨ 定义 llama target
  add_library(llama INTERFACE)
  if(NOT EXISTS ${LLAMA_DIR})
    include(ExternalProject)
    # 下载 & 构建 llama.cpp（路径你可以自定义）
    ExternalProject_Add(
      llama_ext
      GIT_REPOSITORY https://github.com/ggml-org/llama.cpp
      GIT_TAG master
      UPDATE_DISCONNECTED 1
      CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${LLAMA_DIR} -DBUILD_SHARED_LIBS=OFF
                 -DCMAKE_BUILD_TYPE=Release # -DLLAMA_BUILD_COMMON=ON
      PREFIX ${CMAKE_BINARY_DIR}/llama)
    add_dependencies(llama llama_ext)
  endif()

  # 系统库
  find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
  find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)

  list(APPEND GGML_CPU_INTERFACE_LINK_LIBRARIES ${ACCELERATE_FRAMEWORK})
  list(APPEND GGML_METAL_INTERFACE_LINK_LIBRARIES ${FOUNDATION_LIBRARY} ${METAL_FRAMEWORK} ${METALKIT_FRAMEWORK})

  find_package(BLAS REQUIRED)
  list(APPEND GGML_CPU_INTERFACE_LINK_LIBRARIES ${BLAS_LIBRARIES})
  list(APPEND GGML_CPU_INTERFACE_LINK_OPTIONS ${BLAS_LINKER_FLAGS})

  # llama库列表
  foreach(lib ggml-base ggml-blas ggml-cpu ggml-metal ggml llama)
    list(APPEND LLAMA_LIBS ${LLAMA_DIR}/lib/lib${lib}.a)
  endforeach()

  target_include_directories(llama INTERFACE ${LLAMA_DIR}/include)
  target_link_directories(llama INTERFACE ${LLAMA_DIR}/lib)
  target_link_libraries(llama INTERFACE
    ${LLAMA_LIBS}
    ${GGML_CPU_INTERFACE_LINK_LIBRARIES}
    ${GGML_METAL_INTERFACE_LINK_LIBRARIES}
  )
  target_link_options(llama INTERFACE ${GGML_CPU_INTERFACE_LINK_OPTIONS})

endif()
