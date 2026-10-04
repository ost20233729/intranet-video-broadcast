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
 * 一帧画面的旅程（接收端视角，与发送端 video_engine 互为镜像）：
 *
 *   ① 收流 av_read_frame      rtp demuxer 按 SDP 的约定收 UDP 包
 *      （阻塞等待 RTP 数据）    并重组出 H.264 码流包
 *       ↓
 *   ② 解码 decodePacket        H.264 软解成 YUV 帧，再转成 RGB
 *      （H264Decoder 完成）
 *       ↓
 *   ③ 显示 latestFrame_        收流线程写"最新帧"（互斥锁保护），
 *      UI 线程 33ms 定时器轮询取走显示
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

    H264Decoder *decoder_ = nullptr;    // H.264 解码器封装（每次会话重建，见 h264_decoder.h）
    std::thread thread_;                // 收流线程（receiveLoop 在其中阻塞收流）
    std::atomic<bool> stop_{false};     // 停止标志：置位后中断回调立即打断阻塞的收流
    std::atomic<int> framesDecoded_{0}; // 解码帧计数（无头自测/退出统计用）
    QMutex frameMutex_;                 // latestFrame_ 的锁：收流线程写、UI 线程读
    QImage latestFrame_;                // 最新解码帧（只保留最新一帧，旧帧直接覆盖丢弃）
};
