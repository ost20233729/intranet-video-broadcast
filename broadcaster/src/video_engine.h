// ============================================================
// video_engine.h - 发送端引擎：采集 -> 转换 -> 编码 -> 输出
// 职责：把采集到的画面编码成 H.264，输出为：
//   - mp4 文件（录制）
//   - RTP 网络流（推流给接收端）
// 设计：纯 C++ 引擎，不依赖界面；Qt GUI 只是套在外面的壳（见 broadcaster_window）
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
 * 一帧画面的旅程（本类 = 整条流水线，run() 每轮循环走一遍）：
 *
 *   ① 采集 read_frame()        OpenCV 读出 BGR 画面（摄像头/测试图/视频文件）
 *       ↓
 *   ② 转换 init_sws() 提供的   sws_scale 把 BGR "翻译"成 YUV420P
 *      SwsContext              （摄像头说 BGR，x264 只听得懂 YUV）
 *       ↓
 *   ③ 编码 init_encoder() 提供 libx264 软编成 H.264
 *      的 AVCodecContext       （帧送进去，编码好的包取出来）
 *       ↓
 *   ④ 封装 init_muxer() 提供   rtp muxer 把 H.264 切成 RTP 包推 UDP
 *      的 AVFormatContext      （或 mp4 muxer 写成文件）
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
    // ---- 初始化四部曲：init() 里依次调用，任何一步失败即整体失败 ----
    //      注意：init 的顺序是 采集→编码→转换→封装，与运行时"一帧的旅程"
    //      顺序（采集→转换→编码→封装，见类注释）不同——两个"顺序"说的是
    //      两回事：一个是开资源、一个是数据流，别混着记。
    //      每个函数名后标的是它在"旅程"中的位置：
    bool init_capture();                 // 旅程① 采集：打开采集源（摄像头/测试图/文件）
    bool init_encoder();                 // 旅程③ 编码：初始化 libx264 编码器（配宽高/码率/GOP/无B帧）
    bool init_sws();                     // 旅程② 转换：初始化 BGR -> YUV420P 转换器
    bool init_muxer();                   // 旅程④ 封装：初始化输出封装器（mp4 文件 / rtp 网络流）
    // ---- 主循环每轮调用 ----
    void read_frame(cv::Mat& bgr);       // 旅程① 读一帧 BGR 画面（摄像头/文件/测试图，统一 640x480）
    bool encode_and_write(AVFrame* frame); // 旅程③④ 编码一帧并输出（写文件或发网络，内含 SDP 生成）
    // ---- 收尾 ----
    void cleanup();                      // 释放所有 FFmpeg/OpenCV 资源（幂等：重复调用安全）

    // ---- 输入参数（构造时定死，之后不变）----
    std::string source_;                 // 采集源描述："0"=摄像头 / "test"=测试画面 / 其他=视频文件路径
    std::string output_;                 // 输出："xxx.mp4" 写文件 / "rtp://IP:端口" 推网络流
    int duration_sec_;                   // 运行秒数（<=0 表示不限时，跑到外部停止）

    // ---- 控制 ----
    // 停止标志：GUI 线程调 request_stop() 只置这个标志，run() 每轮循环检查后退出。
    // 用 atomic 因为两个线程同时访问：GUI 线程写、工作线程读，普通 bool 会数据竞争。
    std::atomic<bool> stop_requested_;   // 停止请求标志（跨线程读）

    // ---- 采集 ----
    bool use_test_pattern_;              // 是否为测试图模式（采集源 "test" 时为 true）
    cv::VideoCapture cap_;               // OpenCV 采集器（内部打开 /dev/video0 或视频文件）
    int frame_index_;                    // 已采集帧计数（兼作时间戳基准：第 N 帧的显示时刻 = N/30 秒）

    // ---- 转换 ----
    SwsContext* sws_ctx_;                // BGR -> YUV420P 转换器（sws = software scale，软件缩放/转换）
    AVFrame*    frame_yuv_;              // 转换后的 YUV420P 帧（循环外预分配、每帧复用，避免反复 malloc）

    // ---- 编码 ----
    AVCodecContext* enc_ctx_;            // libx264 编码器上下文（保存宽高/码率/GOP 等编码参数）

    // ---- 封装 ----
    bool is_network_output_;             // 是否为 RTP 网络推流（output_ 以 "rtp://" 开头；否则写文件）
    bool sdp_written_;                   // SDP 是否已生成（首个关键帧后生成一次，之后不再重复写）
    AVFormatContext* fmt_ctx_;           // 封装上下文（rtp muxer 推网络流 / mp4 muxer 写文件）
    int stream_index_;                   // 输出流索引（fmt_ctx_ 里第几条流；本项目只有 1 条视频流）
};

#endif /* __VIDEO_ENGINE_H__ */
