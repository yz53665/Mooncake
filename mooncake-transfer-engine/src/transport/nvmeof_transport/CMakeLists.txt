# 基础源文件
set(NVMEOF_BASE_SOURCES "nvmeof_transport.cpp")

# CUDA GDS 相关源文件 (仅在 USE_CUDA=ON 时编译)
set(NVMEOF_CUDA_SOURCES "cufile_context.cpp" "cufile_desc_pool.cpp")

# 合并源文件列表
set(NVMEOF_SOURCES ${NVMEOF_BASE_SOURCES})

if(USE_CUDA)
    list(APPEND NVMEOF_SOURCES ${NVMEOF_CUDA_SOURCES})
endif()

add_library(nvmeof_transport OBJECT ${NVMEOF_SOURCES})

if(USE_CUDA)
    target_include_directories(nvmeof_transport PUBLIC "/usr/local/cuda/include")
endif()

