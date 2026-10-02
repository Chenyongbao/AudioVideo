// 音频采集：ALSA PCM s16le。RV1126 移植时替换为 RKMPI AI 通道 / G.711A 采集。
#include "platform/audio_source.hpp"
#include <alsa/asoundlib.h>
#include <stdexcept>

namespace {

class AlsaSource : public IAudioSource {
public:
    ~AlsaSource() override { close(); }

    bool open(const std::string& dev, uint32_t rate, int channels) override {
        snd_pcm_t* pcm = nullptr;
        if (snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) return false;
        pcm_ = pcm;
        rate_ = rate;
        channels_ = channels;
        // period = 20ms 帧，便于与视频帧对齐做同步
        period_frames_ = rate / 50;
        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_malloc(&hw);
        snd_pcm_hw_params_any(pcm_, hw);
        bool ok = true;
        ok = ok && snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0;
        ok = ok && snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE) >= 0;
        ok = ok && snd_pcm_hw_params_set_channels(pcm_, hw, channels) >= 0;
        unsigned int r = rate;
        ok = ok && snd_pcm_hw_params_set_rate_near(pcm_, hw, &r, nullptr) >= 0;
        snd_pcm_uframes_t pf = period_frames_;
        ok = ok && snd_pcm_hw_params_set_period_size_near(pcm_, hw, &pf, nullptr) >= 0;
        ok = ok && snd_pcm_hw_params(pcm_, hw) >= 0;
        snd_pcm_hw_params_free(hw);
        if (!ok) { close(); return false; }
        if (snd_pcm_prepare(pcm_) < 0) { close(); return false; }
        return true;
    }

    void close() override {
        if (pcm_) { snd_pcm_drop(pcm_); snd_pcm_close(pcm_); pcm_ = nullptr; }
    }

    bool isOpen() const override { return pcm_ != nullptr; }

    bool read(AudioFrame& out) override {
        if (!pcm_) return false;
        out.data.resize((size_t)period_frames_ * channels_ * sizeof(int16_t));
        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, out.data.data(), period_frames_);
        if (n < 0) {                       // underrun/overrun 恢复
            if (snd_pcm_recover(pcm_, (int)n, 1) < 0) return false;
            return false;
        }
        out.samples = (uint32_t)n;
        out.sample_rate = rate_;
        out.pts_ms = now_ms();
        return true;
    }

    uint32_t sampleRate() const override { return rate_; }

private:
    snd_pcm_t* pcm_ = nullptr;
    uint32_t rate_ = 16000;
    int channels_ = 1;
    snd_pcm_uframes_t period_frames_ = 320;
};

} // namespace

std::unique_ptr<IAudioSource> create_alsa_source() {
    return std::make_unique<AlsaSource>();
}
