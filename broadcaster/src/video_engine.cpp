// ============================================================
// video_engine.cpp - 发送端引擎实现（M2/M3）
// 链路：OpenCV 采集(BGR) -> sws_scale 转 YUV420P
//       -> libx264 编码 H.264 -> 输出
//       （M2：mp4 封装写文件；M3：rtp muxer 推 UDP 网络流）
// RTP 分包、UDP 收发均由 FFmpeg 的 rtp muxer 封装完成，无需手写底层
// 参考笔记：https://www.cnblogs.com/linuxAndMcu/category/1613476.html
// ============================================================
#include "params.h" // 两端共享参数（分辨率/帧率/码率/组播地址等，见 common/params.h）
#include "video_engine.h"

#include <chrono>
#include <cstdio>
#include <thread>

// FFmpeg 是 C 库：必须包 extern "C"，否则函数名会被 C++ 修饰(mangle)导致链接失败
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

// 编码参数统一取自 common/params.h（vb::kWidth / kHeight / kFrameRate /
// kBitRate / kGopSize），两端不再各自维护一份，改参只需动一处。

// ------------------------------------------------------------------
// 构造函数：只保存参数，不打开任何资源
// ------------------------------------------------------------------
VideoEngine::VideoEngine(const std::string &source, const std::string &output, int durationSec)
    : source_(source), output_(output), duration_sec_(durationSec), use_test_pattern_(false), frame_index_(0),
      sws_ctx_(nullptr), frame_yuv_(nullptr), enc_ctx_(nullptr), is_network_output_(false), fmt_ctx_(nullptr),
      stream_index_(-1), sdp_written_(false), stop_requested_(false)
{
}

// ------------------------------------------------------------------
// 析构函数：释放所有资源（安全起见再调一次 cleanup，幂等）
// ------------------------------------------------------------------
VideoEngine::~VideoEngine()
{
    cleanup();
}

// ------------------------------------------------------------------
// 请求停止（线程安全）：仅置标志，run() 循环在安全点退出并收尾
// ------------------------------------------------------------------
void VideoEngine::request_stop()
{
    stop_requested_ = true;
}

// ------------------------------------------------------------------
// 初始化：按 采集 -> 编码 -> 转换 -> 封装 顺序打开各环节
// ------------------------------------------------------------------
bool VideoEngine::init()
{
    if (!init_capture())
    {
        return false;
    }
    if (!init_encoder())
    {
        return false;
    }
    if (!init_sws())
    {
        return false;
    }
    if (!init_muxer())
    {
        return false;
    }
    printf("[engine] 初始化完成: %dx%d@%dfps, 输出=%s\n", vb::kWidth, vb::kHeight, vb::kFrameRate, output_.c_str());
    return true;
}

// ------------------------------------------------------------------
// 打开采集源：支持 摄像头("0") / 测试图("test") / 视频文件(路径)
// ------------------------------------------------------------------
bool VideoEngine::init_capture()
{
    if (source_ == "test")
    {
        // 测试图模式：不依赖摄像头，方便当前无 /dev/video0 的环境验证编码链路
        use_test_pattern_ = true;
        printf("[engine] 采集源: 测试画面（无摄像头模式）\n");
        return true;
    }

    if (source_ == "0")
    {
        // 摄像头：打开 /dev/video0
        if (!cap_.open(0))
        {
            fprintf(stderr, "[engine] 打开摄像头失败（摄像头未透传？检查 /dev/video0）\n");
            return false;
        }
        // 尽量设置 640x480；部分摄像头不支持时会自动回退到原生分辨率
        cap_.set(cv::CAP_PROP_FRAME_WIDTH, vb::kWidth);
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, vb::kHeight);
    }
    else
    {
        // 视频文件：方便用测试录像代替摄像头调试
        if (!cap_.open(source_))
        {
            fprintf(stderr, "[engine] 打开视频文件失败: %s\n", source_.c_str());
            return false;
        }
    }
    printf("[engine] 采集源: %s\n", source_.c_str());
    return true;
}

// ------------------------------------------------------------------
// 初始化 libx264 软编编码器（FFmpeg 六函数中的前三个）
// ------------------------------------------------------------------
bool VideoEngine::init_encoder()
{
    // 1. 找编码器：libx264（软编，项目明确不用硬编）
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
    {
        fprintf(stderr, "[engine] 找不到 libx264 编码器（检查 ffmpeg 是否带 x264）\n");
        return false;
    }

    // 2. 分配编码器上下文
    enc_ctx_ = avcodec_alloc_context3(codec);
    if (!enc_ctx_)
    {
        fprintf(stderr, "[engine] avcodec_alloc_context3 失败\n");
        return false;
    }

    // 3. 配置编码参数
    enc_ctx_->width = vb::kWidth;          // 宽
    enc_ctx_->height = vb::kHeight;        // 高
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P; // 输入像素格式（sws 转换后的）
    enc_ctx_->time_base = {1, vb::kFrameRate};  // 时间基准 1/30 秒
    enc_ctx_->framerate = {vb::kFrameRate, 1};  // 帧率 30fps
    enc_ctx_->bit_rate = vb::kBitRate;          // 目标码率 2Mbps
    enc_ctx_->gop_size = vb::kGopSize;          // 短 GOP：每 60 帧一个 I 帧
    enc_ctx_->max_b_frames = 0;             // 不用 B 帧：降低延迟和复杂度
    // GLOBAL_HEADER：SPS/PPS 写进 extradata —— 写 mp4 时进文件头(moov)；
    // RTP 场景 FFmpeg 靠它生成 SDP 的 sprop-parameter-sets，接收端
    // （viewer / VLC / ffplay）用 rtp demuxer 读 SDP 即可拿到参数集解码。
    enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 4. 编码速度预设：ultrafast 软编 CPU 开销最小、延迟最低
    av_opt_set(enc_ctx_->priv_data, "preset", "ultrafast", 0);

    // 5. 打开编码器
    if (avcodec_open2(enc_ctx_, codec, nullptr) < 0)
    {
        fprintf(stderr, "[engine] avcodec_open2 失败\n");
        return false;
    }
    printf("[engine] 编码器: libx264, %dMbps, GOP=%d 帧\n", vb::kBitRate / 1000000, vb::kGopSize);
    return true;
}

// ------------------------------------------------------------------
// 初始化 BGR -> YUV420P 转换器，并分配一帧 YUV420P 供编码用
// ------------------------------------------------------------------
bool VideoEngine::init_sws()
{
    // 输入输出尺寸都是 640x480（read_frame 里已统一 resize）
    sws_ctx_ = sws_getContext(vb::kWidth, vb::kHeight, AV_PIX_FMT_BGR24, vb::kWidth, vb::kHeight,
                              AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_)
    {
        fprintf(stderr, "[engine] sws_getContext 失败\n");
        return false;
    }

    // 分配一帧 YUV420P 用于编码（Y/U/V 三块 buffer 由 FFmpeg 统一分配）
    frame_yuv_ = av_frame_alloc();
    if (!frame_yuv_)
    {
        fprintf(stderr, "[engine] av_frame_alloc 失败\n");
        return false;
    }
    frame_yuv_->format = AV_PIX_FMT_YUV420P;
    frame_yuv_->width = vb::kWidth;
    frame_yuv_->height = vb::kHeight;
    if (av_frame_get_buffer(frame_yuv_, 32) < 0)
    {
        fprintf(stderr, "[engine] av_frame_get_buffer 失败\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------
// 初始化输出封装器
//   - 输出地址是 "rtp://..."：用 FFmpeg 的 rtp muxer 推 UDP 网络流（M3）
//     发包、RTP 分包等底层全由 FFmpeg 完成；并生成 SDP 文件给接收端
//   - 否则：按文件扩展名选封装（如 .mp4），录制到文件（M2）
// ------------------------------------------------------------------
bool VideoEngine::init_muxer()
{
    // 判断输出类型：以 "rtp://" 开头 = 网络推流，否则 = 写文件
    is_network_output_ = (output_.rfind("rtp://", 0) == 0);

    // 1. 创建输出上下文：网络用 rtp 封装器；文件用 nullptr 让它按扩展名自动选
    if (avformat_alloc_output_context2(&fmt_ctx_, nullptr,
                                       is_network_output_ ? "rtp" : nullptr,
                                       output_.c_str()) < 0)
    {
        fprintf(stderr, "[engine] avformat_alloc_output_context2 失败\n");
        return false;
    }

    // 2. 新建输出流
    AVStream *out_stream = avformat_new_stream(fmt_ctx_, nullptr);
    if (!out_stream)
    {
        fprintf(stderr, "[engine] avformat_new_stream 失败\n");
        return false;
    }
    stream_index_ = out_stream->index;

    // 3. 把编码器参数复制给输出流（宽高、像素格式、SPS/PPS 等）
    if (avcodec_parameters_from_context(out_stream->codecpar, enc_ctx_) < 0)
    {
        fprintf(stderr, "[engine] avcodec_parameters_from_context 失败\n");
        return false;
    }
    out_stream->time_base = enc_ctx_->time_base; // 与编码器时间基准一致

    // 4. 非网络输出才需要打开文件（mp4）；RTP muxer 自带 UDP socket，无需打开文件
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&fmt_ctx_->pb, output_.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            fprintf(stderr, "[engine] avio_open 失败: %s\n", output_.c_str());
            return false;
        }
    }

    // 5. 写文件头 / 初始化 RTP 会话（RTP 没有真正的"头"，这里是建立发送参数）
    if (avformat_write_header(fmt_ctx_, nullptr) < 0)
    {
        fprintf(stderr, "[engine] avformat_write_header 失败\n");
        return false;
    }

    if (is_network_output_)
    {
        // RTP 流没有文件可供回放，接收端必须靠"说明书"(SDP)才知道：
        // 编码是什么、RTP payload 号、端口多少。
        // 注意：这里先不生成 SDP——首帧编码前 libx264 的 extradata 可能只有
        // SPS 没有 PPS（实测如此），此时生成的 sprop-parameter-sets 会缺 PPS，
        // 接收端打不开。改为在 encode_and_write 检测到首个关键帧（extradata
        // 已完整）后再写 broadcast.sdp。
        printf("[engine] 推流地址: %s（SDP 将在首个关键帧后生成）\n", output_.c_str());
    }
    else
    {
        printf("[engine] 输出封装: %s\n", output_.c_str());
    }
    return true;
}

// ------------------------------------------------------------------
// 读一帧到 bgr（测试图模式生成画面，否则从采集器读并统一尺寸）
// ------------------------------------------------------------------
void VideoEngine::read_frame(cv::Mat &bgr)
{
    if (use_test_pattern_)
    {
        // 生成测试画面：渐变底 + 移动白色方块 + 帧号，肉眼可确认画面在动
        bgr = cv::Mat(vb::kHeight, vb::kWidth, CV_8UC3);
        for (int y = 0; y < vb::kHeight; y++)
        {
            uchar *row = bgr.ptr<uchar>(y); // 逐行填充渐变色
            for (int x = 0; x < vb::kWidth; x++)
            {
                row[x * 3 + 0] = (uchar)(x * 255 / vb::kWidth);                        // B 随 x 渐变
                row[x * 3 + 1] = (uchar)(y * 255 / vb::kHeight);                       // G 随 y 渐变
                row[x * 3 + 2] = (uchar)((x + y) * 255 / (vb::kWidth + vb::kHeight)); // R
            }
        }
        // 移动的白色方块（横坐标随帧号变化，形成动态画面）
        int bx = (frame_index_ * 8) % vb::kWidth;
        cv::rectangle(bgr, cv::Rect(bx, vb::kHeight / 2, 40, 40), cv::Scalar(255, 255, 255), cv::FILLED);
        // 左上角标注帧号
        char text[32];
        snprintf(text, sizeof(text), "frame %d", frame_index_);
        cv::putText(bgr, text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 2);
    }
    else
    {
        // 摄像头/文件：读一帧
        cap_.read(bgr);
        if (bgr.empty())
        {
            return; // 读不到帧（文件播完 / 摄像头故障）
        }
        // 尺寸不一致时统一缩放到 640x480（保证编码尺寸固定）
        if (bgr.cols != vb::kWidth || bgr.rows != vb::kHeight)
        {
            cv::resize(bgr, bgr, cv::Size(vb::kWidth, vb::kHeight));
        }
    }
}

// ------------------------------------------------------------------
// 编码一帧 YUV 并写入文件
// 关键点：send_frame 送进编码器，receive_packet 取出 H.264 包
// ------------------------------------------------------------------
bool VideoEngine::encode_and_write(AVFrame *frame)
{
    // 1. 把一帧 YUV 送进编码器
    int ret = avcodec_send_frame(enc_ctx_, frame);
    if (ret < 0)
    {
        fprintf(stderr, "[engine] avcodec_send_frame 失败 (%d)\n", ret);
        return false;
    }

    // 2. 取出编码好的 H.264 包并写入文件（一帧可能产生多个包，循环取完）
    // 注意：AVPacket 必须用 av_packet_alloc 分配！avcodec_receive_packet
    //       内部会先 unref 传入的包，栈上裸 AVPacket 会导致释放野指针崩溃
    // 3. 输出流的时间基准（mp4 封装时 muxer 可能把 time_base 改成 1/15360 等）
    AVStream *out_stream = fmt_ctx_->streams[stream_index_];
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc_ctx_, pkt) == 0)
    {
        pkt->stream_index = stream_index_; // 告诉封装器这是第几个流
        // 把 PTS/DTS 从编码器基准(1/30)换算成输出流基准，否则文件时长会算错
        pkt->pts = av_rescale_q_rnd(pkt->pts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, enc_ctx_->time_base, out_stream->time_base);
        // 网络推流：首个关键帧时 libx264 的 extradata 已含完整 SPS/PPS，
        // 此刻生成的 SDP 才能带上完整 sprop-parameter-sets。接收端（viewer /
        // VLC / ffplay）通过 rtp demuxer 读 SDP 拿到参数集解码，流内无需补发
        if (is_network_output_ && (pkt->flags & AV_PKT_FLAG_KEY) && !sdp_written_)
        {
            sdp_written_ = true;
            char sdp[4096];
            if (av_sdp_create(&fmt_ctx_, 1, sdp, sizeof(sdp)) == 0)
            {
                FILE *f = fopen("broadcast.sdp", "w");
                if (f)
                {
                    fputs(sdp, f);
                    fclose(f);
                    printf("[engine] 已生成接收端 SDP: broadcast.sdp（含 sprop-parameter-sets）\n");
                    printf("[engine] 接收端用它收流：./viewer broadcast.sdp  或  ffplay broadcast.sdp\n");
                }
            }
        }
        if (av_interleaved_write_frame(fmt_ctx_, pkt) < 0)
        {
            fprintf(stderr, "[engine] av_interleaved_write_frame 失败\n");
            av_packet_free(&pkt);
            return false;
        }
        av_packet_unref(pkt); // 写完释放包引用
    }
    av_packet_free(&pkt);
    return true;
}

// ------------------------------------------------------------------
// 主循环：采集 -> 转换 -> 编码 -> 写文件，直到按 q / 到时 / 源结束
// ------------------------------------------------------------------
void VideoEngine::run()
{
    printf("[engine] 开始%s（GUI 点停止或 Ctrl+C 可中断）\n",
           is_network_output_ ? "推流" : "采集编码");

    // 推流的"开播时刻"：用于按帧率均匀发送（见循环开头节流）
    const auto send_start = std::chrono::steady_clock::now();

    while (true)
    {
        // 0. 网络推流必须按 30fps 的真实节奏发送：
        //    第 frame_index_ 帧应在 (开播 + frame_index_/30秒) 时发出。
        //    否则全部数据会在瞬间发完，接收端缓存溢出丢包、画面快进。
        //    （若编码跟不上节奏，sleep_until 目标已过则立即发送，不堆积追赶）
        if (is_network_output_)
        {
            auto target = send_start +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>((double)frame_index_ / vb::kFrameRate));
            std::this_thread::sleep_until(target);
        }

        // 1. 读一帧（BGR）
        cv::Mat bgr;
        read_frame(bgr);
        if (bgr.empty())
        {
            printf("[engine] 采集源结束，停止输出\n");
            break;
        }

        // 2. BGR -> YUV420P（sws_scale 转换进 frame_yuv_）
        uint8_t *src_data[4] = {bgr.data, nullptr, nullptr, nullptr};
        int src_linesize[4] = {(int)bgr.step, 0, 0, 0};
        sws_scale(sws_ctx_, src_data, src_linesize, 0, vb::kHeight, frame_yuv_->data, frame_yuv_->linesize);

        // 3. 设置时间戳（按 1/30 秒递增），编码并输出（写文件 / 发网络）
        frame_yuv_->pts = frame_index_;
        if (!encode_and_write(frame_yuv_))
        {
            break;
        }

        // 4. 进度打印：每 30 帧（1 秒）打印一次
        frame_index_++;
        if (frame_index_ % vb::kFrameRate == 0)
        {
            printf("[engine] 已%s %d 帧 (%.1f 秒)\n", is_network_output_ ? "发送" : "编码", frame_index_,
                   (double)frame_index_ / vb::kFrameRate);
        }

        // 5. 退出条件：收到停止请求（GUI 按钮/closeEvent）或达到设定时长
        if (stop_requested_)
        {
            printf("[engine] 收到停止请求，退出\n");
            break;
        }
        if (duration_sec_ > 0 && frame_index_ >= duration_sec_ * vb::kFrameRate)
        {
            printf("[engine] 达到设定时长 %d 秒\n", duration_sec_);
            break;
        }

        // 6. 仅"测试图 + 写文件"模式需要手动延时模拟 30fps：
        //    测试图无采集节奏；摄像头自带节奏；网络模式已在循环开头节流
        if (use_test_pattern_ && !is_network_output_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / vb::kFrameRate));
        }
    }

    // 7. 收尾：flush 编码器（把缓冲里的帧全部吐出来）
    printf("[engine] 正在收尾（flush 编码器）...\n");
    AVStream *out_stream = fmt_ctx_->streams[stream_index_];
    AVPacket *pkt = av_packet_alloc();     // 同上：必须用 alloc 分配
    avcodec_send_frame(enc_ctx_, nullptr); // 送空帧通知编码器结束
    while (avcodec_receive_packet(enc_ctx_, pkt) == 0)
    {
        pkt->stream_index = stream_index_;
        // 同样把时间戳换算到输出流基准
        pkt->pts = av_rescale_q_rnd(pkt->pts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, enc_ctx_->time_base, out_stream->time_base);
        av_interleaved_write_frame(fmt_ctx_, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    // 8. 写文件尾（moov 等元数据）；对 RTP 流基本无操作（流式协议）
    av_write_trailer(fmt_ctx_);

    // 9. 释放资源
    cleanup();
    if (is_network_output_)
    {
        printf("[engine] 推流结束，共发送 %d 帧到 %s\n", frame_index_, output_.c_str());
    }
    else
    {
        printf("[engine] 完成，共编码 %d 帧，输出: %s\n", frame_index_, output_.c_str());
    }
}

// ------------------------------------------------------------------
// 释放所有资源（幂等：重复调用安全）
// ------------------------------------------------------------------
void VideoEngine::cleanup()
{
    if (fmt_ctx_)
    {
        avformat_free_context(fmt_ctx_); // 内部会自动关闭输出文件(pb)
        fmt_ctx_ = nullptr;
    }
    if (enc_ctx_)
    {
        avcodec_free_context(&enc_ctx_);
    }
    if (frame_yuv_)
    {
        av_frame_free(&frame_yuv_);
    }
    if (sws_ctx_)
    {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (cap_.isOpened())
    {
        cap_.release();
    }
    // 显式置空底层 V4L2 句柄，避免部分驱动/虚拟摄像头在 release() 后仍短暂占用设备节点
    cap_ = cv::VideoCapture();
}
