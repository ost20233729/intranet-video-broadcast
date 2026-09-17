# 发送端 broadcaster：Qt 界面（一个开始按钮）-> 采集 -> H.264 编码 -> 输出
#   GUI：无参数启动界面（推流参数契约统一取 common/params.h）
#   CLI：-s/-f/-t 无头模式，回归测试用（与历史命令行兼容）
# 依赖 Qt Widgets（与接收端一致，Qt5/Qt6 由根 CMake 统一探测）
add_executable(broadcaster
    src/broadcaster_window.cpp
    src/main.cpp
    src/video_engine.cpp
)

# ---- 依赖查找 ----
# OpenCV：摄像头采集
find_package(OpenCV REQUIRED)

# FFmpeg：编码(H.264) + 像素转换 + 封装
# Ubuntu 的 FFmpeg 没有 CMake config，统一走 pkg-config
# （需安装 libavcodec-dev / libavformat-dev / libavutil-dev / libswscale-dev）
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)

target_include_directories(broadcaster PRIVATE ${OpenCV_INCLUDE_DIRS})

target_link_libraries(broadcaster PRIVATE
    Qt::Widgets
    vb_common
    ${OpenCV_LIBS}
    PkgConfig::FFMPEG
)
