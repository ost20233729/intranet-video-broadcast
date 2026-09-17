#include "h264_decoder.h"

#include <iostream>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------------------
// RAII 删除器实现：FFmpeg 的 C 资源绝不能直接 delete，必须走各自的配对释放函数。
// operator() 均 const noexcept：unique_ptr 的析构/ reset 都以 noexcept 约束，
// 释放资源本身也不应抛异常或上抛。
// ---------------------------------------------------------------------------
void AvCodecContextDeleter::operator()(AVCodecContext *p) const noexcept
{
    avcodec_free_context(&p);
}

// av_frame_free 内部会先 av_frame_unref(p)，把该帧引用计数所指向的 buffer 都释放掉。
void AvFrameDeleter::operator()(AVFrame *p) const noexcept
{
    av_frame_free(&p);
}

void SwsContextDeleter::operator()(SwsContext *p) const noexcept
{
    sws_freeContext(p);
}

H264Decoder::~H264Decoder() = default; // unique_ptr 成员析构时自动调用各删除器释放 FFmpeg 资源

bool H264Decoder::init(const AVCodecParameters *params)
{
    if (!params)
    {
        std::cerr << "H264Decoder: 未提供流参数" << std::endl;
        return false;
    }

    // 1. 按流参数找解码器（rtp demuxer 解析 SDP 后 codec_id 为 H264）
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        std::cerr << "H264 decoder not found" << std::endl;
        return false;
    }

    // 2. RAII：交给智能指针托管。即便后面 avcodec_open2 失败提前 return
    //    （例如返回负值），codecContext_ 也会在离开作用域时被删除器自动释放。
    codecContext_ = CodecContextPtr(avcodec_alloc_context3(codec));
    if (!codecContext_)
    {
        std::cerr << "Failed to allocate H264 decoder context" << std::endl;
        return false;
    }

    // 3. 复制流参数（含 SDP sprop-parameter-sets 带来的 SPS/PPS extradata）
    if (avcodec_parameters_to_context(codecContext_.get(), params) < 0)
    {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        return false;
    }

    // 4. 打开解码器
    if (avcodec_open2(codecContext_.get(), codec, nullptr) < 0)
    {
        std::cerr << "Failed to initialize H264 decoder" << std::endl;
        return false;
    }
    // 线程数为 0 表示自动选择，线程类型为切片模式，低延迟模式，快速模式
    codecContext_->thread_count = 0;
    codecContext_->thread_type = FF_THREAD_SLICE;
    codecContext_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecContext_->flags2 |= AV_CODEC_FLAG2_FAST;

    frame_    = FramePtr(av_frame_alloc());
    rgbFrame_ = FramePtr(av_frame_alloc());
    if (!frame_ || !rgbFrame_)
    {
        std::cerr << "Failed to allocate H264 decoder resources" << std::endl;
        return false;
    }

    std::cout << "H264 decoder initialized successfully" << std::endl;
    return true;
}

bool H264Decoder::decodePacket(AVPacket *packet)
{
    if (!codecContext_ || !packet || packet->size <= 0)
    {
        return false;
    }

    bool decoded = false;
    if (avcodec_send_packet(codecContext_.get(), packet) >= 0)
    {
        while (true)
        {
            const int ret = avcodec_receive_frame(codecContext_.get(), frame_.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0)
            {
                break;
            }

            if (!ensureRgbFrame(frame_->width, frame_->height))
            {
                av_frame_unref(frame_.get());
                break;
            }

            sws_scale(swsContext_.get(), frame_->data, frame_->linesize,
                      0, frame_->height,
                      rgbFrame_->data, rgbFrame_->linesize);
            latestImage_ = QImage(reinterpret_cast<uchar *>(rgbFrame_->data[0]),
                                  frame_->width, frame_->height,
                                  rgbFrame_->linesize[0], QImage::Format_RGB888)
                               .copy();
            decoded = true;
            av_frame_unref(frame_.get());
        }
    }
    return decoded;
}

const QImage &H264Decoder::latestImage() const
{
    return latestImage_;
}

bool H264Decoder::ensureRgbFrame(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    const int pixelFormat = codecContext_->pix_fmt;
    if (!swsContext_ ||
        currentWidth_ != width || currentHeight_ != height || currentPixFmt_ != pixelFormat)
    {
        // RAII：直接用 reset 替换 swsContext_，旧上下文由删除器自动 sws_freeContext。
        swsContext_ = SwsContextPtr(
            sws_getContext(width, height, codecContext_->pix_fmt,
                           width, height, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!swsContext_)
        {
            return false;
        }
        currentWidth_ = width;
        currentHeight_ = height;
        currentPixFmt_ = pixelFormat;
    }

    // 缓冲仍可用且尺寸/格式匹配时直接复用，避免每帧重建分配。
    if (rgbFrame_->data[0] && rgbFrame_->buf[0] &&
        rgbFrame_->width == width &&
        rgbFrame_->height == height &&
        rgbFrame_->format == AV_PIX_FMT_RGB24)
    {
        return true;
    }

    // 交由 AVFrame 自己持有 RGB 输出缓冲（av_frame_get_buffer 内部会做必要的
    // 对齐分配，并放入 rgbFrame_->buf 引用）。之后释放只需 av_frame_free 即可，
    // 不再需要额外的 av_freep(&data[0])，也消除了"手动块 + 帧体"两条生命周期。
    av_frame_unref(rgbFrame_.get());
    rgbFrame_->width = width;
    rgbFrame_->height = height;
    rgbFrame_->format = AV_PIX_FMT_RGB24;
    return av_frame_get_buffer(rgbFrame_.get(), 1) >= 0;
}
