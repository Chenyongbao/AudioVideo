#pragma once
// 音频采集抽象：PC 版走 ALSA，RV1126 移植时实现为 RKMPI AI 通道(或直接 G.711A)
#include "platform/common.hpp"
#include <string>
#include <memory>

class IAudioSource {
public:
    virtual ~IAudioSource() = default;
    virtual bool open(const std::string& dev, uint32_t sample_rate, int channels) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    // 阻塞读一包 PCM(s16le interleaved)
    virtual bool read(AudioFrame& out) = 0;
    virtual uint32_t sampleRate() const = 0;
};

std::unique_ptr<IAudioSource> create_alsa_source();
