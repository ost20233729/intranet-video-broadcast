# 两端共享的头部-only 库（参数约定、数据结构）
add_library(vb_common INTERFACE)
target_include_directories(vb_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vb_common INTERFACE cxx_std_17)
