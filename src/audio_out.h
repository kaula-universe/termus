// audio_out.h — waveOut 音频输出(兼容性优先, 不依赖 WASAPI Initialize)
// 输出线程内部运行, 通过 FillFn 回调从调用方拉取 f32 交错数据, 内部转 s16
#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

class AudioOut {
public:
    using FillFn = std::function<UINT32(float* dst, UINT32 maxFrames)>;

    AudioOut();
    ~AudioOut();

    bool init(UINT32 sampleRate, WORD channels);
    void shutdown();

    bool start(FillFn fill);
    void stop();

    UINT32 sampleRate() const { return sampleRate_; }
    WORD channels() const { return channels_; }

private:
    void threadLoop();
    void submitBuffer(size_t idx);

    HWAVEOUT hwo_ = nullptr;
    WAVEFORMATEX fmt_{};
    UINT32 sampleRate_ = 44100;
    WORD channels_ = 2;

    static constexpr int kNumBuffers = 4;
    std::vector<WAVEHDR> hdrs_{static_cast<size_t>(kNumBuffers)};
    std::vector<std::vector<short>> bufs_;
    std::vector<float> tmpF32_;  // fill 回调临时缓冲
    size_t nextSubmit_ = 0;
    size_t framesPerBuf_ = 0;

    FillFn fill_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
