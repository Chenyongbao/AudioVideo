#pragma once
// ============================================================================
// 音频采集抽象接口 —— 平台层的"麦克风"。
// PC 版：ALSA 实现（audio_alsa.cpp），PCM s16le；
// RV1126 移植：新增 audio_rkmpi.cpp（RKMPI AI 通道，直接出 G.711A 包，
// 免编码），本接口与中间件层不改。
// 【主时钟约定】音频是整个系统的主时钟（人耳对失步比人眼敏感、音频设备
// 时钟比摄像头晶振稳定），视频 PTS 在板端向音频样本计数对齐 —— 因此
// 本接口的采样率/采样数精度直接影响全局同步质量。
// ============================================================================
#include "platform/common.hpp"
#include <string>
#include <memory>

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // 打开采集设备。dev: ALSA 设备名（"default"/"hw:0,0"）；
    // period 固定 20ms 一包（与视频帧间隔对齐，便于逐帧做同步判断）
    virtual bool open(const std::string& dev, uint32_t sample_rate, int channels) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 阻塞读一包 PCM（s16le 交织，~20ms）。WSL 常无声卡：打开失败属正常，
    // Recorder 会降级为纯视频录制，不阻塞主链路。
    virtual bool read(AudioFrame& out) = 0;
    virtual uint32_t sampleRate() const = 0;
};

// 工厂函数：返回 PC 版 ALSA 实现（板端换 create_rkmpi_audio_source）
std::unique_ptr<IAudioSource> create_alsa_source();
