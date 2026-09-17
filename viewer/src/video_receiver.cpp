#include "video_receiver.h"

#include "h264_decoder.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <chrono>
#include <cstdio>
#include <thread>

VideoReceiver::VideoReceiver(QObject *parent)
    : QObject(parent), decoder_(new H264Decoder)
{
}

VideoReceiver::~VideoReceiver()
{
    stop();
    delete decoder_;
}

QImage VideoReceiver::takeLatestFrame()
{
    QMutexLocker locker(&frameMutex_);
    QImage frame;
    frame.swap(latestFrame_);
    return frame;
}

int VideoReceiver::decodedFrameCount() const
{
    return framesDecoded_.load();
}

int VideoReceiver::interruptCallback(void *opaque)
{
    // FFmpeg 网络读会周期性回调这里；stop_ 置位后立即中断阻塞的收流，便于退出
    const auto *self = static_cast<VideoReceiver *>(opaque);
    return self->stop_.load() ? 1 : 0;
}

void VideoReceiver::start(const QString &sdpPath)
{
    stop(); // 若已有旧线程先停掉
    stop_ = false;
    thread_ = std::thread(&VideoReceiver::receiveLoop, this, sdpPath);
}

void VideoReceiver::stop()
{
    stop_ = true;
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void VideoReceiver::receiveLoop(const QString &sdpPath)
{
    // 发送端停止/重启会形成全新 RTP 会话。若继续沿用旧 demuxer，其内部残留
    // 上一会话的时间戳/RTCP cseq 状态，与新流冲突导致丢包刷屏甚至收流中断。
    // 对策：每次收流中断后用全新 demuxer 重新打开 SDP（decodeFrame 里同步
    // 重建解码器），新会话从头同步——发送端重启后 ~0.5s 内自动恢复，无需
    // 用户重启 viewer。FFmpeg 日志压到 error，隐藏过渡期 "dropping old
    // packet" 类 warning 噪音、保留真错误。
    av_log_set_level(AV_LOG_ERROR);
    const QByteArray path = sdpPath.toUtf8();

    while (!stop_.load())
    {
        const bool opened = openAndReceive(path.constData());
        if (stop_.load())
        {
            break;
        }
        if (!opened)
        {
            // SDP 尚不存在/不可读（发送端还没生成），提示并等待重试
            emit statusChanged("等待信号...");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 重连间隔
    }

    fprintf(stdout, "[viewer] 收流结束，共解码 %d 帧\n", framesDecoded_.load());
    if (stop_.load())
    {
        emit statusChanged("已停止");
    }
}

// ------------------------------------------------------------------
// 单次收流会话：打开 SDP -> rtp demuxer 收流 -> H.264 解码。
// 返回 true 表示成功打开过 SDP（此后流中断由外层重连）；false 表示
// SDP 本身打不开（如文件还不存在），外层会放慢节奏提示"等待信号"。
// ------------------------------------------------------------------
bool VideoReceiver::openAndReceive(const char *sdpPath)
{
    // 1. 分配输入上下文并挂中断回调（让 av_read_frame 阻塞时能被打断退出）
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt)
    {
        emit statusChanged("分配 AVFormatContext 失败");
        return false;
    }
    fmt->interrupt_callback.callback = interruptCallback;
    fmt->interrupt_callback.opaque = this;

    // 2. 打开 SDP：FFmpeg 识别为 rtp demuxer，由它完成 RTP 收包/重组/去抖。
    //    ffplay broadcast.sdp 与这里走的是同一条路径，行为一致。
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0); // 直播低延迟：不做长时间缓存
    // sdp demuxer 解析 SDP 后要内部打开 rtp:// 协议收流，需显式放行，
    // 否则默认白名单(file,crypto,data)会拒绝：Protocol 'rtp' not on whitelist
    av_dict_set(&opts, "protocol_whitelist", "file,udp,rtp,crypto,data", 0);
    if (avformat_open_input(&fmt, sdpPath, nullptr, &opts) < 0)
    {
        av_dict_free(&opts);
        fprintf(stderr, "[viewer] 打开 SDP 失败: %s\n", sdpPath);
        avformat_free_context(fmt);
        return false; // SDP 缺失/不可读：外层提示等待信号并重试
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        emit statusChanged("解析 SDP 流信息失败");
        avformat_close_input(&fmt);
        return true; // SDP 能打开但解析失败：外层会重连（参数坏时应人工处理）
    }

    // 3. 找视频流，用其参数（含 SDP sprop-parameter-sets 解析出的 extradata）
    //    初始化解码器 —— 这就是过去手写 RTP 时要自己维护 SPS/PPS 的地方。
    //    每次会话都重新 init：解码器用 RAII 持有资源，旧资源自动释放。
    AVStream *videoStream = nullptr;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i)
    {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = fmt->streams[i];
            break;
        }
    }
    if (!videoStream)
    {
        emit statusChanged("SDP 中未找到视频流");
        avformat_close_input(&fmt);
        return true;
    }
    if (!decoder_->init(videoStream->codecpar))
    {
        emit statusChanged("H.264 解码器初始化失败");
        avformat_close_input(&fmt);
        return true;
    }

    emit statusChanged("已接收流，开始解码");

    // 4. 收流解码循环（阻塞直到退出/流结束）
    AVPacket *pkt = av_packet_alloc();
    while (!stop_.load())
    {
        const int readRet = av_read_frame(fmt, pkt);
        if (readRet == AVERROR(EAGAIN))
        {
            // 暂无可读包（如新会话刚启动/空窗期），稍候继续，不视为错误
            av_packet_unref(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (readRet < 0)
        {
            break; // EOF / 错误 / 被中断回调打断 -> 外层重连
        }
        if (pkt->stream_index == videoStream->index && decoder_->decodePacket(pkt))
        {
            const int n = ++framesDecoded_;
            {
                QMutexLocker locker(&frameMutex_);
                latestFrame_ = decoder_->latestImage();
            }
            if (n == 1 || n % 60 == 0)
            {
                fprintf(stdout, "[viewer] 已解码 %d 帧\n", n);
                emit statusChanged(QString("解码中，已解 %1 帧").arg(n));
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // 流结束后清空最后一帧，避免界面长期冻结在旧画面上误导用户；
    // 若只是发送端短暂中断，重连成功收到新帧后画面自动恢复。
    {
        QMutexLocker locker(&frameMutex_);
        latestFrame_ = QImage();
    }

    avformat_close_input(&fmt);
    return true;
}
