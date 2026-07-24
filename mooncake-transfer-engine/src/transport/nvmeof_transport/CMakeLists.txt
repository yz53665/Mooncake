# 基础源文件
set(NVMEOF_BASE_SOURCES "nvmeof_transport.cpp")

# CUDA GDS 相关源文件 (仅在 USE_CUDA=ON 时编译)
set(NVMEOF_CUDA_SOURCES "cufile_context.cpp" "cufile_desc_pool.cpp")

# NDS 相关源文件 (仅在 USE_NDS=ON 时编译)
set(NVMEOF_NDS_SOURCES "nds_desc_pool.cpp")

# 合并源文件列表
set(NVMEOF_SOURCES ${NVMEOF_BASE_SOURCES})

if(USE_CUDA)
    list(APPEND NVMEOF_SOURCES ${NVMEOF_CUDA_SOURCES})
endif()

if(USE_NDS)
    list(APPEND NVMEOF_SOURCES ${NVMEOF_NDS_SOURCES})
endif()

add_library(nvmeof_transport OBJECT ${NVMEOF_SOURCES})

if(USE_CUDA)
    target_include_directories(nvmeof_transport PUBLIC "/usr/local/cuda/include")
endif()

# NDS (NPU Data Service) support
if(USE_NDS)
    # Link to actual libnds.so from CCDK output
    find_library(NDS_LIB nds HINTS "/home/ssu/code/CCDK/output" NO_DEFAULT_PATH)
    if(NDS_LIB)
        message(STATUS "Found NDS library: ${NDS_LIB}")
        target_link_libraries(nvmeof_transport PUBLIC ${NDS_LIB} ascendcl)
    else()
        message(WARNING "NDS library not found at /home/ssu/code/CCDK/output/libnds.so. NDS support will be limited.")
    endif()
endif()


