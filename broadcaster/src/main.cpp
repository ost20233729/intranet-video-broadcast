// ============================================================
// main.cpp - 发送端（broadcaster）入口
//
// 两种运行形态（同一可执行文件）：
//   1) GUI 模式（默认，无参数）：弹出一个 Qt 小窗口，一个"开始"
//      按钮，点击即推流到组播地址（参数契约见 common/params.h）
//        ./broadcaster                 启动界面
//        ./broadcaster --autostart     启动后自动点"开始"（无头自测）
//        ./broadcaster -t 10           10 秒后自动关窗停止（自测）
//   2) CLI 模式（传了 -s/-f/-t）：无界面命令行推流/录制（回归测试用）
//        ./broadcaster -s test -f rtp://239.255.0.1:5004 -t 10
// ============================================================
#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "broadcaster_window.h"
#include "video_engine.h"

// ------------------------------------------------------------------
// 打印命令行用法
// ------------------------------------------------------------------
static void print_usage(const char *prog)
{
    printf("用法: %s [GUI选项] | [-s 采集源] [-f 输出] [-t 秒数]\n", prog);
    printf("GUI 模式（无 -s/-f/-t 参数时默认）:\n");
    printf("  (无参数)        启动界面，点击\"开始\"推流到组播地址\n");
    printf("  --autostart     界面启动后自动点击\"开始\"（无头自测用）\n");
    printf("  -t 秒数         运行指定秒数后自动停止并退出（与 --autostart 搭配自测）\n");
    printf("CLI 模式（无界面，适合无头回归/调试）:\n");
    printf("  -s 0        使用摄像头 /dev/video0（默认）\n");
    printf("  -s test     生成测试画面（无摄像头时验证编码链路）\n");
    printf("  -s 文件路径 使用视频文件作为采集源\n");
    printf("  -f out.mp4  录制到 mp4 文件（默认 out.mp4）\n");
    printf("  -f rtp://IP:端口  推 RTP 网络流，如 rtp://239.255.0.1:5004\n");
    printf("  -t 10       运行 10 秒后自动退出（0 = 持续到外部停止/Ctrl+C）\n");
}

// ------------------------------------------------------------------
// 入口：先纯 C 扫描参数，决定走 GUI（QApplication）还是 CLI（QCoreApplication）
// ------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // ---- 手动扫描参数（不依赖 Qt，便于先决定用哪个 Application 类）----
    std::string source = "0";       // 采集源：默认摄像头
    std::string output = "out.mp4"; // 输出文件（CLI 模式）
    int duration_sec = 0;           // 运行秒数：0 = 不限时
    bool autostart = false;         // GUI：启动即自动点"开始"
    bool want_cli = false;          // 是否 CLI 无头模式

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
        {
            source = argv[++i];
            want_cli = true;
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
        {
            output = argv[++i];
            want_cli = true;
        }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            // -t 同时支持 GUI（自动关窗）与 CLI（到时退出）：
            // 是否进 CLI 只由 -s/-f 决定，仅 -t 时视为 GUI 自测时长
            duration_sec = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--autostart") == 0)
        {
            autostart = true;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            printf("未知参数: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // ---- CLI 模式：无界面，保留回归测试能力 ----
    if (want_cli)
    {
        QCoreApplication app(argc, argv);
        VideoEngine engine(source, output, duration_sec);
        if (!engine.init())
        {
            fprintf(stderr, "[broadcaster] 初始化失败，请检查采集源与输出路径\n");
            return 1;
        }
        engine.run(); // 持续到 -t 到时 / 源结束 / Ctrl+C
        return 0;
    }

    // ---- GUI 模式：QApplication + 开始按钮 ----
    QApplication app(argc, argv);
    BroadcasterWindow window;
    window.show();

    if (autostart)
    {
        QTimer::singleShot(300, &window, &BroadcasterWindow::toggleStream);
    }
    if (duration_sec > 0)
    {
        // 到时自动关窗：closeEvent 内会先请求停止再收尾
        QTimer::singleShot(duration_sec * 1000, &window, &QWidget::close);
    }
    return app.exec();
}
