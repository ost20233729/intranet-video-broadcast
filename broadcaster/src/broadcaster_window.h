// ============================================================
// broadcaster_window.h - 发送端 Qt 界面（与接收端 viewer 同风格）
// 界面只放一个"开始/停止"按钮：点击开始推流，再点停止
// 推流参数固定取 common/params.h 契约：test 画面 ->
// rtp://<组播地址>:<端口>（H.264 640x480@30fps）
// ============================================================
#pragma once

#include <QMainWindow>

#include <atomic>
#include <thread>

class QPushButton;
class VideoEngine;

class BroadcasterWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BroadcasterWindow(QWidget *parent = nullptr);
    ~BroadcasterWindow() override;

    /// 点击"开始/停止"按钮的同一槽（也供 --autostart 自动化自测调用）
    void toggleStream();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    /// 等待推流线程结束并复位界面（在 GUI 线程调用）
    void finishStreaming();

    QPushButton *button_ = nullptr; // 唯一交互按钮：开始 <-> 停止
    VideoEngine *engine_ = nullptr; // 仅 GUI 线程在槽内创建/销毁
    std::thread worker_;            // 推流工作线程（engine_->run() 阻塞其中）
    std::atomic<bool> running_{false};
};
