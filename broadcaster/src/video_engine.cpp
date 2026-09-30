// ============================================================
// video_engine.cpp - 发送端引擎实现（M2/M3）
// 链路：OpenCV 采集(BGR) -> sws_scale 转 YUV420P
//       -> libx264 编码 H.264 -> 输出
//       （mp4 封装写文件；rtp muxer 推 UDP 网络流）
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
// init_capture 的功能：打开采集源，三种来源——
// 摄像头（"0"）/ 测试图（"test"）/ 视频文件（路径）。
// ------------------------------------------------------------------
bool VideoEngine::init_capture()
{
    if (source_ == "test")
    {
        // 测试图模式：不依赖摄像头，方便当前无 /dev/video0 的环境验证编码链路。
        // 画面内容在 read_frame() 里现画（渐变底 + 移动白块 + 帧号），
        // 好处：肉眼能确认"画面在动"，没有屏幕的环境也能靠解码帧数验证链路。
        use_test_pattern_ = true;
        printf("[engine] 采集源: 测试画面（无摄像头模式）\n");
        return true;
    }

    
    if (source_ == "0")
    {
        // 摄像头：打开 /dev/video0
        // open(0) 的 0 = 第 0 号摄像头（Linux 下就是 /dev/video0 这个设备）。
        // VideoCapture 内部通过 Linux 的摄像头驱动把画面读进内存里的 Mat，
        // 之后 cap_.read() 就是从驱动取一帧。
        if (!cap_.open(0))
        {
            fprintf(stderr, "[engine] 打开摄像头失败（摄像头未透传？检查 /dev/video0）\n");
            return false;
        }
        // 尽量设置 640x480；部分摄像头不支持时会自动回退到原生分辨率
        // CAP_PROP_FRAME_WIDTH/HEIGHT = 期望输出帧宽/高（OpenCV 定义的一个属性编号）。
        // 这里只是"请求"，驱动不支持时实际拿到的是原生尺寸——
        // 所以 read_frame() 里还有 resize 兜底（见 read_frame）。
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
// 初始化 libx264 软编编码器
// FFmpeg 编码器使用套路共六步，本函数完成前五步（第 6 步在 encode_and_write）：
//   1. 找编码器   avcodec_find_encoder_by_name（本项目找的是 "libx264"）
//   2. 分配上下文 avcodec_alloc_context3
//   3. 配置参数   宽高/像素格式/时间基准/码率/GOP...
//   4. 速度预设   av_opt_set（x264 专属选项，不是每个编码器都有）
//   5. 打开编码器 avcodec_open2
//   6. 送帧收包   avcodec_send_frame + avcodec_receive_packet（encode_and_write 里）
// 下面代码里的 "// 1." ~ "// 5." 就对应这里的步骤编号。
// ------------------------------------------------------------------
bool VideoEngine::init_encoder()
{
    // 1. 找编码器。这行的作用：在 FFmpeg 里查"libx264"这个编码器，
    //    查到后拿到一个指针，后面第 2 步（分配上下文）和第 5 步
    //    （打开编码器）都要用到它。
    //    libx264 是软编（靠 CPU 算的编码器，硬编 = 靠显卡/专用芯片算；
    //    信创机器显卡杂，选软编兼容性最好）。
    //    FFmpeg 和 x264 的关系：FFmpeg 是管流程的平台，真正的 H.264
    //    压缩算法是 x264 这个独立库做的，FFmpeg 通过统一接口调用它。
    // 函数名拆解：avcodec = FFmpeg 的编解码库（av = audio/video）；
    //   find_encoder_by_name = 按名字在编码器注册表里查找。
    // 返回值指向 FFmpeg 库内部的全局只读对象，不要对它 free——
    // 不是你分配的内存（C 的规矩：谁分配谁释放）。
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
    {
        fprintf(stderr, "[engine] 找不到 libx264 编码器（检查 ffmpeg 是否带 x264）\n");
        return false;
    }

    // 2. 分配编码器上下文
    // 上下文 = 编码器的工作台：保存参数、内部状态和缓冲。
    // 参数：编码器指针；分配时按该编码器的默认参数先填好一份。
    enc_ctx_ = avcodec_alloc_context3(codec);
    if (!enc_ctx_)
    {
        fprintf(stderr, "[engine] avcodec_alloc_context3 失败\n");
        return false;
    }

    // 3. 配置编码参数（就是拧工作台上的旋钮）
    enc_ctx_->width = vb::kWidth;          // 宽：640
    enc_ctx_->height = vb::kHeight;        // 高：480
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P; // 输入像素格式：告诉编码器"送进来的帧
    // 是什么格式"——它按这个格式去解读帧数据（像电报要用对密码本）。
    // 为什么是 YUV420P：后面第 3 步会建一个转换环节（init_sws），
    // 专门把摄像头画面转成 YUV420P 送进来——这里和那里约定一致。

    // time_base = 时间基准 = "时间刻度尺上 1 格代表多少秒"。
    // {1, 30} 即 1/30 秒一格：每帧间隔恰好 1 格，
    // 所以后面时间戳直接拿帧号赋值即可（见 run() 里 pts = frame_index_）。
    // 和下面 framerate 的关系：30 帧/秒 → 每帧间隔 1/30 秒 → 刻度取 1/30。
    enc_ctx_->time_base = {1, vb::kFrameRate};  // 时间基准 1/30 秒
    enc_ctx_->framerate = {vb::kFrameRate, 1};  // 帧率 30fps（{30,1} = 30/1 帧每秒）
    enc_ctx_->bit_rate = vb::kBitRate;          // 目标码率 2Mbps：告诉编码器"平均每秒
    // 输出约 250KB"——每帧具体给多少由编码器内部的码率控制分配（概念见 params.h）
    enc_ctx_->gop_size = vb::kGopSize;          // 短 GOP：每 60 帧一个 I 帧（约 2 秒）
    enc_ctx_->max_b_frames = 0;             // 不用 B 帧：B 帧要等"未来帧"才编码 = 引入延迟

    // GLOBAL_HEADER：让编码器把 SPS/PPS 放进 extradata 字段（编码器
    // 上下文里专门放"说明书"类额外信息的字段），而不是随视频流重复发送。
    // SPS/PPS = H.264 的"解码说明书"（分辨率等解码必需的信息）。
    // 设了这个标志后说明书只发一次：写 mp4 时进文件头（moov = mp4 的文件头部分）；
    // 推流时写进 SDP，接收端读 SDP 就能拿到（推流/RTP 见 params.h）。
    // 代价：接收端必须先拿到 SDP，拿不到就黑屏——VLC 黑屏故事的根因。
    enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 4. 编码速度预设：x264 有"速度 vs 压缩率"的档位旋钮，preset 就是它。
    //   档位从最快到最慢（ultrafast 到 veryslow）：越快单帧编码越省时间，
    //   但压缩率越差（同码率下画质略低）。广播类场景优先低延迟，选最快档。
    // av_opt_set 的作用：设"x264 私有选项"——第 3 步设的宽高/码率/GOP
    // 是 FFmpeg 给所有编码器准备的通用旋钮，而 preset 是 x264 独有的
    // 旋钮，通用旋钮里没有它，所以走这个函数单独设。
    // av_opt_set 函数名拆解：av(FFmpeg 核心) opt(选项) set(设置)。
    // 参数说明：第一个参数 = "去哪里设"。x264 的选项不在 FFmpeg 的通用
    //   上下文里，而在它自己的一块私有内存中；enc_ctx_->priv_data 这个
    //   "挂钩"指向那里。传它 = 到 x264 的私有区域里设选项。
    //   第二个参数 "preset" = 设哪个旋钮（速度档位，见上）。
    av_opt_set(enc_ctx_->priv_data, "preset", "ultrafast", 0);

    // 5. 打开编码器：按上面配置好的参数真正初始化 x264，失败返回负值。
    // 三个参数：第一个 enc_ctx_ = 前几步配好的参数（宽高/码率/GOP 都在里面）；
    //   第二个 codec = 这些参数是配给哪个编码器的——就是第 1 步找到的
    //   libx264；第三个 nullptr = 可选的附加选项，这里没有。
    // （avcodec_open2 的 2 = 第 2 版打开接口）
    if (avcodec_open2(enc_ctx_, codec, nullptr) < 0)
    {
        fprintf(stderr, "[engine] avcodec_open2 失败\n");
        return false;
    }
    printf("[engine] 编码器: libx264, %dMbps, GOP=%d 帧\n", vb::kBitRate / 1000000, vb::kGopSize);
    return true;
}

// ------------------------------------------------------------------
// init_sws 的功能：建好"翻译官"环节，两件事：
// ① 建 BGR→YUV 转换器（sws_getContext）
// ② 备一块 YUV 帧空间（转换结果存放处，循环复用）
// ------------------------------------------------------------------
bool VideoEngine::init_sws()
{
    // sws_getContext 的作用：创建 BGR→YUV 的转换器（"翻译官"），
    // 后面的 sws_scale 靠它做转换。参数逐组说明
    // （sws = software scale，软件缩放/像素格式转换库）：
    //   640, 480, BGR24      = 源：640x480 的 BGR24（OpenCV Mat 的格式，注意是 BGR 不是 RGB）
    //   640, 480, YUV420P    = 目标：同尺寸的 YUV420P（x264 只吃这个格式）
    //   SWS_BILINEAR         = 采样算法：双线性插值（简单说：新像素取周围几个像素的平均值），速度与质量折中
    //   三个 nullptr         = 三个可选的附加项，这里都不需要
    // 输入输出尺寸都是 640x480（read_frame 里已统一 resize，所以这里无缩放、只有格式转换）
    sws_ctx_ = sws_getContext(vb::kWidth, vb::kHeight, AV_PIX_FMT_BGR24, vb::kWidth, vb::kHeight,
                              AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_)
    {
        fprintf(stderr, "[engine] sws_getContext 失败\n");
        return false;
    }

    // 分配一帧 YUV420P 用于编码：就是备一块空间，存放 BGR→YUV 的转换结果，
    // init 时建好、循环里每帧复用（结果覆盖写），避免每帧申请/释放内存。
    // （Y/U/V 三块 buffer 由 FFmpeg 统一分配）
    // av_frame_alloc 只分配"帧结构体"本身（存格式/尺寸/时间戳等元信息），
    // 真正装像素的内存要由下面的 av_frame_get_buffer 按格式分配。
    // 为什么分两步：一帧也允许"借用"别人已分配好的内存——数据已经躺在
    // 某块现成内存里时，帧可以直接指向它，不用再拷贝一份新的。
    // 本项目用不到这种借用，老实走"自己分配"的两步。
    frame_yuv_ = av_frame_alloc();
    if (!frame_yuv_)
    {
        fprintf(stderr, "[engine] av_frame_alloc 失败\n");
        return false;
    }
    frame_yuv_->format = AV_PIX_FMT_YUV420P; // 声明这帧的像素格式
    frame_yuv_->width = vb::kWidth;          // 声明宽高（决定 Y/U/V 各占多少内存）
    frame_yuv_->height = vb::kHeight;
    
    // 第二个参数 32 = 内存对齐要求（FFmpeg 惯例值，先不用深究）
    if (av_frame_get_buffer(frame_yuv_, 32) < 0)
    {
        fprintf(stderr, "[engine] av_frame_get_buffer 失败\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------
// init_muxer 干一件大事：建好"输出环节"——决定编码好的 H.264 往哪送、
// 以什么形式送（存 mp4 文件 还是 RTP 推流），并把送出通道建好。
// 像餐厅：后厨做好菜（编码），这一步决定堂食（写文件）还是外卖（推
// 网络），然后把渠道开起来。五步：判断堂食/外卖 → 建工作台 →
// 登记菜（新建流）→ 给菜写说明（复制参数）→ 开店（开文件/建通道）。
// ------------------------------------------------------------------
bool VideoEngine::init_muxer()
{
    // 判断输出类型：output_ 以 "rtp://" 开头 = 网络推流，否则 = 写文件。
    // rfind(str, 0) = 只在字符串开头找 str，找到就返回 0。
    // output_ 来自构造时的参数：CLI 是 -f 后面的串，GUI 是组播地址+端口拼的。
    is_network_output_ = (output_.rfind("rtp://", 0) == 0);

    // 1. 创建输出上下文——后面整个封装环节的"工作台"：记录用哪种
    //    封装格式、有几条流、写到哪。之后第 2~5 步的新建流、打开文件、
    //    写文件头、写帧数据全都在它上面操作（结果存到 &fmt_ctx_）。
    // 三个参数：第二个 = 指定封装格式（"rtp"），传 nullptr 就按第三个
    //    参数的扩展名（.mp4）自己猜；第三个 = 输出地址（文件名或 URL）。
    // 函数名拆解：avformat(封装格式库) alloc(分配) output_context2(输出上下文)。
    if (avformat_alloc_output_context2(&fmt_ctx_, nullptr,
                                       is_network_output_ ? "rtp" : nullptr,
                                       output_.c_str()) < 0)
    {
        fprintf(stderr, "[engine] avformat_alloc_output_context2 失败\n");
        return false;
    }

    // 2. 新建输出流。流 = 一串连续的内容：刷视频时画面是一串（一帧
    //    接一帧）、声音是另一串（一秒接一秒），两串同时播——每串就是
    //    一条流。本行 = 告诉封装器"我要开这样一串"；本项目只发画面
    //    这一串，所以只开这一条。这一串具体是什么，第 3 步才填。
    AVStream *out_stream = avformat_new_stream(fmt_ctx_, nullptr);
    if (!out_stream)
    {
        fprintf(stderr, "[engine] avformat_new_stream 失败\n");
        return false;
    }
    stream_index_ = out_stream->index;

    // 3. 把编码器参数复制给输出流——给这条流填"详细说明"（宽高、
    //    像素格式、SPS/PPS 等）。这些信息最终写进文件头/SDP，
    //    对面的接收端靠它们才知道怎么解码。
    //    codecpar = 流上专门放编解码参数的字段。
    if (avcodec_parameters_from_context(out_stream->codecpar, enc_ctx_) < 0)
    {
        fprintf(stderr, "[engine] avcodec_parameters_from_context 失败\n");
        return false;
    }
    out_stream->time_base = enc_ctx_->time_base; // 与编码器时间基准一致

    // 4. 功能：写 mp4 要先打开输出文件才能写；RTP 自带 UDP socket 不用开。
    //    AVFMT_NOFILE = 封装格式"不需要文件"的标志，用来区分这两种情况。
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&fmt_ctx_->pb, output_.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            fprintf(stderr, "[engine] avio_open 失败: %s\n", output_.c_str());
            return false;
        }
    }

    // 5. 功能：让封装器"开张"——写 mp4 时，先在文件开头写一小段"目录"
    //    （moov），记录这是什么格式、有哪些流、什么参数，播放器打开文件
    //    先读它才知道怎么播；推 RTP 时没有文件，就把 UDP 发送通道建好。
    //    之后每一帧用 av_interleaved_write_frame 输出（见 encode_and_write）。
    if (avformat_write_header(fmt_ctx_, nullptr) < 0)
    {
        fprintf(stderr, "[engine] avformat_write_header 失败\n");
        return false;
    }

    if (is_network_output_)
    {
        // 注意：这里先不生成 SDP——刚启动时编码器的 SPS/PPS 还不齐全，
        // 生成的 SDP 接收端打不开；改到首个关键帧后再写（见 encode_and_write）。
        printf("[engine] 推流地址: %s（SDP 将在首个关键帧后生成）\n", output_.c_str());
    }
    else
    {
        printf("[engine] 输出封装: %s\n", output_.c_str());
    }
    return true;
}

// ------------------------------------------------------------------
// read_frame 的功能是做尺寸调整：取一帧画面（画测试图 / 摄像头 / 视频文件），
// 尺寸不对时缩放成 640x480。不做颜色转换——那是 run() 里 sws_scale 的活。
// ------------------------------------------------------------------
void VideoEngine::read_frame(cv::Mat &bgr)
{
    if (use_test_pattern_)
    {
        // 生成测试画面：渐变底 + 移动白色方块 + 帧号，肉眼可确认画面在动
        // Mat = OpenCV 的图像容器（本质是矩阵）。CV_8UC3 拆解：
        //   CV_8U = 每个分量 8 位无符号数(0-255)，C3 = 3 个通道（BGR）。
        bgr = cv::Mat(vb::kHeight, vb::kWidth, CV_8UC3);
        for (int y = 0; y < vb::kHeight; y++)
        {
            uchar *row = bgr.ptr<uchar>(y); // 取第 y 行首地址；ptr = 直接访问像素内存
            for (int x = 0; x < vb::kWidth; x++)
            {
                // 每个像素占 3 个连续字节，OpenCV 的排列顺序是 B,G,R（不是 RGB）
                row[x * 3 + 0] = (uchar)(x * 255 / vb::kWidth);                        // B 随 x 渐变
                row[x * 3 + 1] = (uchar)(y * 255 / vb::kHeight);                       // G 随 y 渐变
                row[x * 3 + 2] = (uchar)((x + y) * 255 / (vb::kWidth + vb::kHeight)); // R
            }
        }
        // 移动的白色方块（横坐标随帧号变化，形成动态画面）
        // 每帧右移 8 像素，% 宽度 = 取模环绕，移出右边缘后从左边缘回来
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
        // read() 内部：从驱动/文件取一帧并解码成 BGR Mat；取不到时 bgr 为空。
        cap_.read(bgr);
        if (bgr.empty())
        {
            return; // 读不到帧（文件播完 / 摄像头故障）
        }
        // 尺寸不一致时统一缩放到 640x480（保证编码尺寸固定）
        // cols/rows = 列数(宽)/行数(高)。视频文件分辨率可能是任意值，
        // 但编码器初始化时锁定了 640x480，不 resize 就会尺寸不符。
        if (bgr.cols != vb::kWidth || bgr.rows != vb::kHeight)
        {
            cv::resize(bgr, bgr, cv::Size(vb::kWidth, vb::kHeight));
        }
    }
}

// ------------------------------------------------------------------
// encode_and_write 的功能：把一帧 YUV 编码成 H.264，写进文件或发上网络。
// 流程：
// ① 送帧（send_frame）：把 YUV 画面投递给编码器，它收下后自己慢慢编
// ② 收包（receive_packet 循环）：把编好的 H.264 包一个个取出来
//    （一帧可能产出多个包），每个包做三件事：
//     a. 盖时间戳（编码器的尺子换算成封装器的尺子）
//     b. 推流中且是首个关键帧 → 顺手生成 SDP（此刻 SPS/PPS 才齐全）
//     c. 写出去（写文件还是发网络，封装器自动决定）
// ------------------------------------------------------------------
bool VideoEngine::encode_and_write(AVFrame *frame)
{
    // 套路第 6 步之"送帧"：把一帧 YUV 投递给编码器。编码像流水线：
    // 送进去、内部缓冲，结果由下一步 receive_packet 取出。
    // 返回 0 = 收下了；<0 = 失败（内部满了就先收包再送）。
    int ret = avcodec_send_frame(enc_ctx_, frame);
    if (ret < 0)
    {
        fprintf(stderr, "[engine] avcodec_send_frame 失败 (%d)\n", ret);
        return false;
    }

    // 套路第 6 步之"收包"：取出编码好的 H.264 包并写入文件（一帧可能产生多个包，循环取完）
    // receive_packet 每次取一个包：返回 0 = 取到了；AVERROR(EAGAIN) = 暂时没有
    // （要继续 send_frame 才有新包），所以用 while 循环一直取到 EAGAIN 为止。
    // 注意：AVPacket 必须用 av_packet_alloc 分配！avcodec_receive_packet
    //       内部会先 unref 传入的包，栈上裸 AVPacket 会导致释放野指针崩溃
    // 输出流的时间基准可能和编码器不同（mp4 封装时 muxer 可能把
    // time_base 改成 1/15360 等）——所以下面才要做时间戳换算。
    AVStream *out_stream = fmt_ctx_->streams[stream_index_];
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc_ctx_, pkt) == 0)
    {
        pkt->stream_index = stream_index_; // 告诉封装器这是第几个流
        // 功能：把时间戳从编码器的尺子（1/30 秒一格）换算到输出流的尺子
        // （mp4 封装器会换尺子），否则文件时长会算错。
        // PTS = "该在什么时刻显示"，DTS = "该在什么时刻解码"；
        // av_rescale_q 就是按比例换算两把尺子的函数。
        pkt->pts = av_rescale_q_rnd(pkt->pts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, enc_ctx_->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, enc_ctx_->time_base, out_stream->time_base);
        // 功能：推到首个关键帧时生成 SDP——此刻 SPS/PPS 才齐全，接收端
        // 读它就能解码。AV_PKT_FLAG_KEY = 这个包是关键帧（I 帧）的标志。
        if (is_network_output_ && (pkt->flags & AV_PKT_FLAG_KEY) && !sdp_written_)
        {
            sdp_written_ = true;
            char sdp[4096];
            // av_sdp_create 让 FFmpeg 按当前会话自动生成 SDP 文本；
            // 第二个参数 1 = 只描述 1 条流（本项目只有视频流）。
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
        // 把包写出去（写文件或发网络）。interleaved = 交错：多流时按时间戳
        // 交错排列输出，保证播放顺序正确；单流时等价于普通写。
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
// run() 的功能：主循环——每轮四件事：
// ① 取一帧（read_frame）
// ② 转格式（sws_scale：BGR → YUV420P）
// ③ 盖时间戳（第 N 帧 = N/30 秒）
// ④ 编码送出（encode_and_write）
// 推流时每轮开头按 30fps 节流；结束条件：停止请求 / 到时 / 源结束；
// 收尾：flush 编码器 + 写文件尾。
// ------------------------------------------------------------------
void VideoEngine::run()
{
    printf("[engine] 开始%s（GUI 点停止或 Ctrl+C 可中断）\n",
           is_network_output_ ? "推流" : "采集编码");

    // 推流的"开播时刻"：用于按帧率均匀发送（见循环开头节流）
    const auto send_start = std::chrono::steady_clock::now();

    while (true)
    {
        // 每轮开头（推流时）：按 30fps 的真实节奏发——每帧睡到它的
        // "预定发射时刻"再发，否则数据瞬间发完、接收端溢出丢包。
        // 第 N 帧的时刻 = 开播 + N/30 秒；sleep_until 睡到该时刻：
        // 编码慢了自动少睡，晚点了立即发、不追赶。
        if (is_network_output_)
        {
            auto target = send_start +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>((double)frame_index_ / vb::kFrameRate));
            std::this_thread::sleep_until(target);
        }

        // ① 取一帧（BGR）
        cv::Mat bgr;
        read_frame(bgr);
        if (bgr.empty())
        {
            printf("[engine] 采集源结束，停止输出\n");
            break;
        }

        // ② 转格式：BGR → YUV420P（翻译官干活，结果写进 frame_yuv_）。
        // 参数要点：源是 BGR 三通道连续数据（只填一个指针），目标是
        // Y/U/V 三块分开的平面；从第 0 行处理到第 480 行。
        uint8_t *src_data[4] = {bgr.data, nullptr, nullptr, nullptr};
        int src_linesize[4] = {(int)bgr.step, 0, 0, 0};
        sws_scale(sws_ctx_, src_data, src_linesize, 0, vb::kHeight, frame_yuv_->data, frame_yuv_->linesize);

        // ③ 盖时间戳（按 1/30 秒递增）。为什么帧号可直接当 PTS：
        // time_base = 1/30 秒 = "1 格 = 1 帧时长"，第 N 帧 = N 格 = N/30 秒。
        frame_yuv_->pts = frame_index_;
        // ④ 编码送出（encode_and_write：写文件 / 发网络）
        if (!encode_and_write(frame_yuv_))
        {
            break;
        }

        // 每 30 帧（1 秒）打印一次进度
        frame_index_++;
        if (frame_index_ % vb::kFrameRate == 0)
        {
            printf("[engine] 已%s %d 帧 (%.1f 秒)\n", is_network_output_ ? "发送" : "编码", frame_index_,
                   (double)frame_index_ / vb::kFrameRate);
        }

        // 退出条件：收到停止请求（GUI 按钮/closeEvent）或达到设定时长
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

        // 仅"测试图 + 写文件"模式需要手动延时模拟 30fps：
        // 测试图无采集节奏；摄像头自带节奏；网络模式已在循环开头节流
        if (use_test_pattern_ && !is_network_output_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / vb::kFrameRate));
        }
    }

    // 收尾：flush 编码器——编码器内部有延迟缓冲（凑 GOP、存参考帧），
    //    停推时还有几帧没输出；送一个空帧(nullptr) = 告诉编码器
    //    "没有新帧了，把缓冲里的全吐出来"，之后 receive_packet 循环
    //    把这些剩余帧取完。不 flush 视频尾部会丢帧。
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
    // 写文件尾：mp4 在这里补全 moov 索引等元数据（缺它文件打不开/拖进度条会坏）；
    //    对 RTP 流基本无操作（流式协议没有"文件尾"的概念）
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
// cleanup 的功能：释放全部 FFmpeg/OpenCV 资源（幂等：重复调用安全，
// 每个指针用完置 nullptr）。顺序：先关封装器，再放编码器/帧/转换器。
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
