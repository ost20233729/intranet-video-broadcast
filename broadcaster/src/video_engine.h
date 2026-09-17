// ============================================================
// video_engine.h - 发送端引擎：采集 -> 转换 -> 编码 -> 输出
// 职责：把采集到的画面编码成 H.264，输出为：
//   - mp4 文件（M2，录制）
//   - RTP 网络流（M3，推流给接收端）
// 设计：纯 C++ 命令行引擎，不依赖界面；阶段三再套 Qt
// ============================================================
#ifndef __VIDEO_ENGINE_H__
#define __VIDEO_ENGINE_H__

#include <opencv2/opencv.hpp>
#include <atomic>
#include <string>

// FFmpeg 对象前向声明（实现细节放 .cpp，头文件保持轻量）
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * @brief 发送端视频引擎：采集 -> 像素转换 -> H.264 编码 -> 输出
 *
 * 采集源支持：
 *   - "0"        摄像头 /dev/video0
 *   - "test"     生成测试画面（无摄像头时验证编码链路）
 *   - 其他字符串 视频文件路径
 *
 * 输出支持（根据 output 参数自动选择）：
 *   - "xxx.mp4"                 录制到 mp4 文件（M2）
 *   - "rtp://IP:端口"            推 RTP 网络流（M3），如 rtp://127.0.0.1:5004
 */
class VideoEngine
{
public:
    /**
     * @brief 构造函数：只保存参数，不打开任何资源
     * @param source      采集源：摄像头 / 测试图 / 视频文件（见类注释）
     * @param output      输出：mp4 文件路径 或 rtp://IP:端口 地址
     * @param durationSec 运行秒数（<=0 表示持续运行到按 q）
     */
    VideoEngine(const std::string& source,
                const std::string& output,
                int durationSec);

    /// @brief 析构函数：确保所有资源被释放
    ~VideoEngine();

    /**
     * @brief 请求安全停止（线程安全，可随时从其他线程调用）。
     *        run() 会在下一个循环迭代处退出并完成收尾（flush 编码器等）。
     */
    void request_stop();

    /**
     * @brief 初始化：打开采集源、编码器、转换器、输出文件
     * @retval true  成功
     * @retval false 失败（错误原因打印到 stderr）
     */
    bool init();

    /**
     * @brief 采集-编码-输出 主循环（阻塞直到时长结束或按 q）
     */
    void run();

private:
    bool init_capture();                 // 打开采集源（摄像头/测试图/文件）
    bool init_encoder();                 // 初始化 libx264 编码器
    bool init_sws();                     // 初始化 BGR -> YUV420P 转换器
    bool init_muxer();                   // 初始化输出封装器（mp4 文件 / rtp 网络流）
    void read_frame(cv::Mat& bgr);       // 读一帧（摄像头/文件/测试图）
    bool encode_and_write(AVFrame* frame); // 编码一帧并输出（写文件或发网络）
    void cleanup();                      // 释放所有资源

    // ---- 输入参数 ----
    std::string source_;                 // 采集源描述
    std::string output_;                 // 输出：mp4 路径 或 rtp://IP:端口
    int duration_sec_;                   // 运行秒数（<=0 无限）

    // ---- 控制 ----
    std::atomic<bool> stop_requested_;   // 停止请求标志（跨线程读）

    // ---- 采集 ----
    bool use_test_pattern_;              // 是否为测试图模式
    cv::VideoCapture cap_;               // OpenCV 采集器
    int frame_index_;                    // 已采集帧计数（兼作 PTS 基准）

    // ---- 转换 ----
    SwsContext* sws_ctx_;                // BGR -> YUV420P 转换器
    AVFrame*    frame_yuv_;              // 转换后的 YUV420P 帧

    // ---- 编码 ----
    AVCodecContext* enc_ctx_;            // libx264 编码器上下文

    // ---- 封装 ----
    bool is_network_output_;             // 是否为 RTP 网络推流（否则写文件）
    bool sdp_written_;                   // SDP 是否已生成（首个关键帧后生成一次）
    AVFormatContext* fmt_ctx_;           // 封装上下文（mp4 muxer / rtp muxer）
    int stream_index_;                   // 输出流索引
};

#endif /* __VIDEO_ENGINE_H__ */
