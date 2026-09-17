// ============================================================
// 视频广播项目 - 两端共享参数约定（占位文件）
// ============================================================
//
// 分辨率: 640x480
// 帧率:   30 fps
// 编码:   H.264
// 传输:   RTP over UDP
// 端口:   5004
//
// 后续在这里补充双方协商好的常量定义、数据结构等
// ============================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace vb
{
inline constexpr int kWidth = 640;
inline constexpr int kHeight = 480;
inline constexpr int kFrameRate = 30;
inline constexpr int kBitRate = 2'000'000;
inline constexpr int kGopSize = 60;

inline constexpr uint16_t kUdpPort = 5004;
inline constexpr std::string_view kDefaultMulticastAddress = "239.255.0.1";
inline constexpr int kPayloadTypeH264 = 96;

inline constexpr int kRtpHeaderSize = 12;
inline constexpr int kRtpMaxPacketSize = 1400;

struct RtpHeader
{
    uint8_t v_p_x; // version(2), padding(1), extension(1), csrc count(4)
    uint8_t marker_payloadType;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
};

} // namespace vb
