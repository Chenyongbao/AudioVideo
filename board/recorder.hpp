// ============================================================================
// 板端录像模块:H.264 → fMP4(mov/moof 分片)+ 预录环形缓冲
// 设计:
//   - 预录:非录制态把 H.264 包存进内存环形缓冲(deque,上限 PRE_SECONDS);
//     START 时先把缓冲里"过去 30s"灌进文件,再继续写实时包
//   - 断电安全:mov 立即写盘 + 每隔 frag_ms 写一个 moof 分片并 flush,
//     进程被杀/断电只丢最后一个未完成分片,已写内容均可播(无需 moov 补写)
//   - 用 libavformat(板上 libavformat.so.58 现成)封装,输出 faststart 前置头
// ============================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <deque>
#include <vector>
#include <mutex>

class Recorder {
public:
    ~Recorder();

    // 每个编码包都喂这里(录制中写盘,非录制入环形缓冲)
    // key: 是否关键帧;ts_ms: 单调时间戳(用于缓冲过期与打 pts)
    void feed(const uint8_t *p, uint32_t len, bool key, uint64_t ts_ms);

    // 开始录像:先把环形缓冲(过去 N 秒)写进文件,再写实时流
    // extradata: SPS/PPS(fMP4 empty_moov 必需,否则解码器报 non-existing PPS)
    bool start(const std::string &path, int width, int height, int fps,
               const uint8_t *extradata, uint32_t extradata_len);
    // 停止:写 trailer 并关闭
    void stop();

    bool recording() const;
    uint64_t recordedMs() const;          // 已录时长(含预录)
    std::string currentPath() const;      // 当前录像文件路径(未录制为空)
    // 事件标记:录制中把 "相对毫秒 epoch 文本" 追加到 <录像>.meta,
    // 回放端按相对毫秒在时间轴上画 ▲ 并支持跳转
    bool mark(const std::string &text);
    static constexpr int PRE_SECONDS = 30;

    const std::string &lastError() const { return err_; }

private:
    void drainPrebuffer();                // 把环形缓冲灌进文件
    bool openFile(const std::string &path);
    void writePacket(const uint8_t *p, uint32_t len, bool key, uint64_t ts_ms);
    void closeFile();

    struct AVFormatContext *fmt_ = nullptr;
    int stream_idx_ = -1;
    bool header_written_ = false;

    std::mutex m_;
    struct PreEntry {
        std::vector<uint8_t> bytes;
        bool key;
        uint64_t ts_ms;
    };
    std::deque<PreEntry> pre_;               // 预录环形缓冲(带关键帧标志与时间戳)
    size_t pre_bytes_ = 0;
    bool key_seen_ = false;                  // 缓冲内是否有关键帧(回灌起点)

    bool recording_ = false;
    uint64_t start_ts_ = 0;                  // 起录时刻(单调 ms)
    uint64_t last_ts_ = 0;
    std::string path_;                       // 当前录像文件路径(mark 写 .meta 用)
    std::string err_;
};
