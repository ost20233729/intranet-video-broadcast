#pragma once

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

class H264Decoder;

/**
 * @brief 接收端网络接收器
 *
 * 职责：读取发送端生成的 SDP（broadcast.sdp），交给 FFmpeg rtp demuxer
 * 收流并解码 H.264，把最新一帧交给 UI 线程显示。
 *
 * 架构：start() 启动一条独立收流线程（av_read_frame 阻塞等待 RTP 数据），
 * 解码出的图像通过互斥锁保护的 latestFrame_ 供主线程轮询读取；状态信息
 * 通过 statusChanged 信号（跨线程队列投递）通知 UI。
 */
class VideoReceiver : public QObject
{
    Q_OBJECT

public:
    explicit VideoReceiver(QObject *parent = nullptr);
    ~VideoReceiver() override;

    /**
     * @brief 启动收流线程
     * @param sdpPath 发送端生成的 SDP 文件路径（默认 broadcast.sdp）
     */
    void start(const QString &sdpPath);

    /// @brief 请求停止收流并等待线程退出（幂等）
    void stop();

    /// @brief 取走最新一帧（无新帧时返回空图像）
    QImage takeLatestFrame();

    /// @brief 已成功解码的帧数（退出统计用）
    int decodedFrameCount() const;

signals:
    void statusChanged(const QString &message);

private:
    void receiveLoop(const QString &sdpPath);
    bool openAndReceive(const char *sdpPath);
    static int interruptCallback(void *opaque);

    H264Decoder *decoder_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> framesDecoded_{0};
    QMutex frameMutex_;
    QImage latestFrame_;
};
