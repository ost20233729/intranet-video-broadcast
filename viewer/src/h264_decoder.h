#pragma once

#include <QImage>

#include <memory>

struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// ---------------------------------------------------------------------------
// RAII 删除器：FFmpeg 的 C 资源不能用 delete，必须调用各自配对的释放函数。
// 每种结构体的释放方式不同，因此需要各自的专用删除器。
// 声明放在头文件里是为了让 std::unique_ptr 成员类型在类声明处可见可实例化；
// operator() 的实现放在 h264_decoder.cpp（那里才有 FFmpeg 头）。
// ---------------------------------------------------------------------------
struct AvCodecContextDeleter
{
    void operator()(AVCodecContext *p) const noexcept;
};

struct AvFrameDeleter
{
    void operator()(AVFrame *p) const noexcept;
};

struct SwsContextDeleter
{
    void operator()(SwsContext *p) const noexcept;
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using FramePtr        = std::unique_ptr<AVFrame, AvFrameDeleter>;
using SwsContextPtr   = std::unique_ptr<SwsContext, SwsContextDeleter>;

class H264Decoder
{
public:
    H264Decoder() = default;
    ~H264Decoder();

    /**
     * @brief 用流参数初始化解码器。
     * 参数来自 rtp demuxer 打开 SDP 后的 codecpar：其中 extradata 由 SDP 的
     * sprop-parameter-sets 填充（含 SPS/PPS），解码器开播即具备完整参数。
     */
    bool init(const AVCodecParameters *params);

    /// @brief 解码一帧 H.264 数据（来自 av_read_frame 的 AVPacket）
    bool decodePacket(AVPacket *packet);

    const QImage &latestImage() const;

private:
    bool ensureRgbFrame(int width, int height);

    // 用 RAII 智能指针持有 FFmpeg 资源：所有权唯一、析构自动调用配对释放。
    CodecContextPtr codecContext_;
    FramePtr        frame_;
    FramePtr        rgbFrame_;
    SwsContextPtr   swsContext_;
    QImage latestImage_;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    int currentPixFmt_ = -1;
};
