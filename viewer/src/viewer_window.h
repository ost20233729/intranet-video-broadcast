#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>

class QLabel;
class QTimer;
class VideoReceiver;

class ViewerWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ViewerWindow(const QString &sdpPath, QWidget *parent = nullptr);
    ~ViewerWindow() override;

    /// @brief 本次会话累计解码帧数（无头自测/退出统计用）
    int decodedFrameCount() const;

private slots:
    void updateImage();
    void updateStatus(const QString &message);

private:
    QLabel *label_ = nullptr;
    VideoReceiver *receiver_ = nullptr;
    QTimer *displayTimer_ = nullptr;
};
