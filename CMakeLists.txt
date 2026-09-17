cmake_minimum_required(VERSION 3.16)
project(LowLatencyIntranetVideoBroadcast VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---- Qt 版本探测（Qt6 / Qt5 均支持，便于两台机器环境不同）----
# 1) 优先用用户显式指定的 Qt：cmake -DCMAKE_PREFIX_PATH=...
# 2) 否则自动找本机常见的 Qt 安装位置（~/Qt 下的 Qt6、系统 apt 的 Qt5/Qt6）
if(NOT DEFINED CMAKE_PREFIX_PATH)
    file(GLOB _qt6_candidates
        "$ENV{HOME}/Qt/6.*/gcc_64"
        "/opt/Qt/6.*/gcc_64")
    if(_qt6_candidates)
        list(SORT _qt6_candidates ORDER DESCENDING)
        list(GET _qt6_candidates 0 _qt_latest)
        list(PREPEND CMAKE_PREFIX_PATH "${_qt_latest}")
    endif()
endif()

# 视频广播两端实际只用到 Widgets（QApplication/QMainWindow/QLabel/QImage）
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

message(STATUS "LowLatencyIntranetVideoBroadcast: 使用 Qt${QT_VERSION_MAJOR} (${Qt${QT_VERSION_MAJOR}_DIR})")

add_subdirectory(common)
add_subdirectory(broadcaster)
add_subdirectory(viewer)
