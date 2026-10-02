#pragma once
// 分段写文件抽象：一个分段 = 一个独立可播的 fragmented MP4 文件。
// RV1126 移植时可替换为 MP4V2/自研分片封装，接口不变。
#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

class ISegmentWriter {
public:
    virtual ~ISegmentWriter() = default;
    virtual bool open(const char* path, int width, int height, int fps,
                      int has_audio, uint32_t audio_rate) = 0;
    virtual bool start(const AVCodecParameters* vpar, const AVCodecParameters* apar) = 0;
    virtual bool writeVideo(const AVPacket* pkt, AVRational tb) = 0;
    virtual bool writeAudio(const AVPacket* pkt, AVRational tb) = 0;
    virtual bool close() = 0;
};

std::unique_ptr<ISegmentWriter> create_fmp4_writer();
