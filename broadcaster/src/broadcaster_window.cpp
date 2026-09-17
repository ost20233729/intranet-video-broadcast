// ============================================================
// broadcaster_window.cpp - 发送端 Qt 界面实现
// 线程模型：
//   GUI 线程：建窗口、点按钮、管理 VideoEngine 生命周期
//   工作线程：跑阻塞的 VideoEngine::run()（采集-编码-推流）
//   停止：GUI 线程调 engine_->request_stop()，工作线程在安全点
//   退出并完成收尾，GUI 线程 join 后销毁 engine
// ============================================================
#include "broadcaster_window.h"

#include "params.h"
#include "video_engine.h"

#include <QCloseEvent>
#include <QDir>
#include <QFont>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <string>
#include <thread>

BroadcasterWindow::BroadcasterWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Broadcaster");
    resize(380, 220);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    // 顶部信息：让使用者知道这一键会推什么流、SDP 落在哪里
    const std::string url = std::string("rtp://") + std::string(vb::kDefaultMulticastAddress) +
                            ":" + std::to_string(vb::kUdpPort);
    auto *info = new QLabel(central);
    info->setAlignment(Qt::AlignCenter);
    info->setWordWrap(true);
    info->setText(QString("采集源：优先摄像头（无摄像头自动用测试画面）\n分辨率 %1x%2 @ %3 fps\n推流地址: %4\n（SDP 将生成在当前目录 broadcast.sdp）")
                      .arg(vb::kWidth)
                      .arg(vb::kHeight)
                      .arg(vb::kFrameRate)
                      .arg(QString::fromStdString(url)));

    button_ = new QPushButton("开始", central);
    button_->setMinimumHeight(48);
    button_->setFont(QFont(button_->font().family(), button_->font().pointSize() + 2));
    connect(button_, &QPushButton::clicked, this, &BroadcasterWindow::toggleStream);

    layout->addWidget(info);
    layout->addWidget(button_);
    setCentralWidget(central);

    statusBar()->showMessage("就绪");
}

BroadcasterWindow::~BroadcasterWindow()
{
    finishStreaming();
}

void BroadcasterWindow::closeEvent(QCloseEvent *event)
{
    // 关窗前先请求停止，避免 join 卡死（run() 收尾很快）
    if (running_ && engine_)
    {
        engine_->request_stop();
    }
    finishStreaming();
    event->accept();
}

void BroadcasterWindow::toggleStream()
{
    if (running_)
    {
        // ---- 停止推流 ----
        button_->setEnabled(false);
        button_->setText("正在停止…");
        engine_->request_stop(); // 线程安全：工作线程下一个循环退出并收尾
        finishStreaming();       // join（run() 收尾很快，不会明显卡界面）
        statusBar()->showMessage("推流已停止");
        return;
    }

    // ---- 开始推流 ----
    // 采集源策略：优先摄像头（/dev/video0），打开失败自动回退测试画面，
    // 保证无摄像头环境也能一键演示整条链路。编码/网络参数全部取自
    // common/params.h，改一处两端同步生效。
    const std::string url = std::string("rtp://") + std::string(vb::kDefaultMulticastAddress) +
                            ":" + std::to_string(vb::kUdpPort);
    auto *engine = new VideoEngine("0", url, 0); // 优先摄像头
    QString sourceDesc = "摄像头 /dev/video0";
    if (!engine->init())
    {
        delete engine; // init() 失败时内部资源已按顺序释放
        engine = new VideoEngine("test", url, 0); // 回退：测试画面
        if (!engine->init())
        {
            delete engine;
            QMessageBox::critical(this, "Broadcaster", "初始化失败，请查看终端输出。");
            return;
        }
        sourceDesc = "测试画面（未检测到可用摄像头）";
    }
    engine_ = engine;
    running_ = true;
    button_->setText("停止");
    statusBar()->showMessage(QString("推流中 → %1\n采集源: %2\nSDP: %3")
                                 .arg(QString::fromStdString(url))
                                 .arg(sourceDesc)
                                 .arg(QDir::current().absoluteFilePath("broadcast.sdp")));

    worker_ = std::thread([this, engine] {
        engine->run(); // 阻塞：按 30fps 节奏采集-编码-推流，直至停止/出错
        running_ = false;
        // 回主线程复位界面（线程自然结束时由 stop 标志/源结束触发）
        QMetaObject::invokeMethod(this,
                                  [this] {
                                      if (worker_.joinable())
                                      {
                                          worker_.join(); // 线程已结束，立即返回
                                      }
                                      delete engine_;
                                      engine_ = nullptr;
                                      running_ = false;
                                      button_->setEnabled(true);
                                      button_->setText("开始");
                                      statusBar()->showMessage("推流已结束（查看终端输出确认原因）");
                                  },
                                  Qt::QueuedConnection);
    });
}

void BroadcasterWindow::finishStreaming()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
    delete engine_; // run() 内部已 cleanup 全部 FFmpeg/OpenCV 资源，这里释放外壳
    engine_ = nullptr;
    running_ = false;
    button_->setEnabled(true);
    button_->setText("开始");
    // 给 V4L2 摄像头驱动/VMware 透传设备一点释放时间，降低再次打开失败概率
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}
