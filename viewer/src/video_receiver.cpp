#include "video_receiver.h"

#include "h264_decoder.h"

// FFmpeg 是 C 语言写的库。extern "C" 告诉 C++ 编译器：这些头文件里的
// 函数声明按 C 规则处理、名字原样不修饰——C++ 支持函数重载，默认会给
// 函数名加"尾巴"，不加这段声明链接时就找不到库里的原函数名。
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

// takeLatestFrame 的功能：UI 线程每 33ms 调一次，把最新一帧取走显示；
// 没有新帧时返回空图像（取走即清空，不会重复显示同一帧）。
QImage VideoReceiver::takeLatestFrame()
{
    QMutexLocker locker(&frameMutex_);
    QImage frame;
    frame.swap(latestFrame_);
    return frame;
}

// decodedFrameCount 的功能：返回已解码帧数（无头自测/退出统计用）。
int VideoReceiver::decodedFrameCount() const
{
    return framesDecoded_.load();
}

// interruptCallback 的功能：回调函数——自己不调它，把地址交给 FFmpeg
// （openAndReceive 里 interrupt_callback.callback = 这行），FFmpeg 阻塞
// 等网络数据时会周期性反过来调它：stop_ 置位就返回 1 = 请立刻中断。
// 为什么参数是 void *opaque：它是 static 函数，没有 this、访问不了成员；
// 交地址时把 this 塞进 opaque，FFmpeg 调用时原样传回，这里转回
// VideoReceiver 指针再读 stop_。
int VideoReceiver::interruptCallback(void *opaque)
{
    // FFmpeg 网络读会周期性回调这里；stop_ 置位后立即中断阻塞的收流，便于退出
    const auto *self = static_cast<VideoReceiver *>(opaque);
    return self->stop_.load() ? 1 : 0;
}

// start 的功能：启动收流线程（sdpPath = SDP 文件路径）；已有旧线程先停掉。
void VideoReceiver::start(const QString &sdpPath)
{
    stop(); // 若已有旧线程先停掉
    stop_ = false;
    thread_ = std::thread(&VideoReceiver::receiveLoop, this, sdpPath);
}

// stop 的功能：请求停止收流并等线程退出（幂等，重复调用安全）。
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
    // receiveLoop 的功能：收流线程的主循环 = "外层重连循环"——
    // 打开 SDP 收流 → 流中断 → 睡 500ms → 再用全新 demuxer 重开。
    // 为什么要全新重开：旧会话内部状态会和新流冲突；重开后发送端
    // 重启约 0.5 秒内自动恢复，用户不用重启 viewer。
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
// openAndReceive 的功能：单次收流会话——打开 SDP → 收流 → 解码，
// 直到流中断或停止。打开过 SDP 返回 true（中断交给外层重连）；
// SDP 打不开（如文件还不存在）返回 false，外层提示"等待信号"。
// ------------------------------------------------------------------
bool VideoReceiver::openAndReceive(const char *sdpPath)
{
    // 1. 功能：分配输入上下文并挂中断回调（阻塞收流时能被打断退出）。
    // 先手工分配而不是直接 open——要提前把中断回调挂上去。
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt)
    {
        emit statusChanged("分配 AVFormatContext 失败");
        return false;
    }
    // 中断回调 = 逃生舱：阻塞的网络读会周期性调它，返回 1 = 立刻中断。
    // opaque 跟着回调一起携带数据（这里传 this 读 stop_ 标志）——没有它，
    // stop() 时 av_read_frame 堵在网络读上，线程杀不掉。
    fmt->interrupt_callback.callback = interruptCallback;
    fmt->interrupt_callback.opaque = this;

    // 2. 打开 SDP：FFmpeg 识别为 rtp demuxer，由它完成 RTP 收包/重组/去抖。
    //    ffplay（FFmpeg 自带的命令行播放器）打开 broadcast.sdp 与这里走的
    //    是同一条路径，行为一致。
    // AVDictionary = FFmpeg 的"键值对选项袋"（C 库没有 map，用它传可选项）。
    AVDictionary *opts = nullptr;
    // av_dict_set 参数：&opts 袋子指针；"fflags" 键；"nobuffer" 值；最后的 0 是附加选项，这里不用。
    av_dict_set(&opts, "fflags", "nobuffer", 0); // 直播低延迟：不做长时间缓存
    // sdp demuxer 解析 SDP 后要内部打开 rtp:// 协议收流，需显式放行，
    // 否则默认白名单(file,crypto,data)会拒绝：Protocol 'rtp' not on whitelist
    // （排查 VLC 黑屏时用过的同一个知识点，ffplay 的 -protocol_whitelist 同理）
    av_dict_set(&opts, "protocol_whitelist", "file,udp,rtp,crypto,data", 0);
    // avformat_open_input 的作用：打开输入（这里就是打开 SDP 文件，
    // 解析出流信息）。参数：第一个 &fmt = 上下文（结果填进它）；
    //   第二个 sdpPath = 要打开的"输入"（SDP 文件路径）；
    //   第三个 nullptr = 不指定格式，自动探测；
    //   第四个 &opts = 选项袋。成功后 fmt 里就有了流信息。
    // 注意它是"阻塞打开"：网络不通时会卡住——所以前面要先挂中断回调。
    if (avformat_open_input(&fmt, sdpPath, nullptr, &opts) < 0)
    {
        av_dict_free(&opts);
        fprintf(stderr, "[viewer] 打开 SDP 失败: %s\n", sdpPath);
        avformat_free_context(fmt);
        return false; // SDP 缺失/不可读：外层提示等待信号并重试
    }
    av_dict_free(&opts);

    // avformat_find_stream_info = 探测流信息：读几包分析出编码和参数，
    // 填充 fmt->streams[] 里每条流的 codecpar（编解码参数块）。
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        emit statusChanged("解析 SDP 流信息失败");
        avformat_close_input(&fmt);
        return true; // SDP 能打开但解析失败：外层会重连（参数坏时应人工处理）
    }

    // 3. 功能：找到视频流，用它的参数（宽高/格式/SPS/PPS）初始化解码器。
    //    接收端的 SPS/PPS 从哪来：SDP 的 sprop-parameter-sets 被解析后
    //    放进流的 codecpar 里——"参数集走 SDP"的接收端闭环。
    //    每次会话都重新 init，旧解码器资源自动释放。
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
    // av_read_frame = 读下一个包：rtp demuxer 在内部 recvfrom 等 UDP 数据，
    // 收到后重组成一帧 H.264 放入 pkt；阻塞等待时靠中断回调逃生。
    AVPacket *pkt = av_packet_alloc();
    while (!stop_.load())
    {
        const int readRet = av_read_frame(fmt, pkt);
        if (readRet == AVERROR(EAGAIN))
        {
            // EAGAIN = "暂时没数据"（如新会话刚启动/空窗期），稍候继续，不视为错误。
            // 区别于后面的 <0：那才是错误/EOF，要跳出循环走外层重连。
            av_packet_unref(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (readRet < 0)
        {
            break; // EOF / 错误 / 被中断回调打断 -> 外层重连
        }
        // stream_index 过滤：SDP 可能声明多条流，只处理视频流那条
        if (pkt->stream_index == videoStream->index && decoder_->decodePacket(pkt))
        {
            const int n = ++framesDecoded_;
            {
                // QMutexLocker = RAII 锁：构造时加锁、离开作用域自动解锁
                //（即使中间异常/提前返回也不会忘解锁）。这里只保留最新一帧，
                // UI 线程 33ms 轮询时旧帧早已被覆盖——视频场景天然允许丢帧。
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
