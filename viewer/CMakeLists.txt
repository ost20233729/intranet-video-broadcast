# 接收端 viewer：读取 SDP → FFmpeg rtp demuxer 收流 → H.264 解码 → Qt 显示
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)

add_executable(viewer
    src/main.cpp
    src/h264_decoder.cpp
    src/viewer_window.cpp
    src/video_receiver.cpp
)

target_link_libraries(viewer PRIVATE
    Qt::Widgets
    PkgConfig::FFMPEG
    vb_common
)
