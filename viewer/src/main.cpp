#include "viewer_window.h"

#include <QApplication>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <cstdio>

// 用法:
//   ./viewer                      默认读当前目录 broadcast.sdp，持续运行
//   ./viewer 路径/xx.sdp          指定 SDP 文件
//   ./viewer -t 10                运行 10 秒后自动退出（无头自测用）
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QString sdpPath = "broadcast.sdp";
    int runSeconds = 0;
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i)
    {
        if (args[i] == "-t" && i + 1 < args.size())
        {
            runSeconds = args[++i].toInt();
        }
        else
        {
            sdpPath = args[i];
        }
    }

    ViewerWindow window(sdpPath);
    window.show();

    if (runSeconds > 0)
    {
        QTimer::singleShot(runSeconds * 1000, &window, &QWidget::close);
    }
    const int code = app.exec();
    printf("[viewer] 退出，共解码 %d 帧\n", window.decodedFrameCount());
    return code;
}
