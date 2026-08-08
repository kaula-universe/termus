// player.h — FFmpeg 解码 + 环形缓冲 + WASAPI 输出
#pragma once

#include "audio_out.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVFilterGraph;
struct AVFilterContext;

// 10 段均衡器(ISO 频点 31Hz-16kHz)
constexpr int kEqBands = 10;
constexpr float kEqFreqs[kEqBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

class Player {
public:
    enum class State { Stopped, Playing, Paused };
    enum class Mode { Sequential, ListRepeat, SingleRepeat, Shuffle, ShuffleOnce };

    Player();
    ~Player();

    // 音频输出初始化(失败时播放静默)
    bool initOutput();

    void setPlaylist(std::vector<std::string> files);
    void play(size_t index);
    void toggle();
    void pause();
    void resume();
    void stop();
    void seek(double seconds);
    void next();
    void prev();
    void cycleMode();
    Mode mode() const;
    const char* modeName() const;

    void setVolume(int pct);
    int volume() const;

    // 变速(0.5x - 2.0x, 保调)
    void setSpeed(float speed);
    float speed() const;

    // 均衡器: 设置第 band 段增益(dB, -12..+12)
    void setEqBand(int band, float db);
    float eqBand(int band) const;
    void setEqEnabled(bool on);
    bool eqEnabled() const;
    void setEqPreset(const float* gainsDb);  // 10 个值

    State state() const;
    size_t currentIndex() const;
    bool audioOk() const { return audioOk_; }
    double position() const;   // 秒
    double duration() const;   // 秒
    std::string currentTitle() const;
    const std::vector<std::string>& playlist() const;

    // 当前曲目信息(信息栏)
    std::string codecName() const;
    int sampleRate() const;
    int bitDepth() const;
    int64_t bitRate() const;

private:
    void decoderLoop();
    bool openCurrent();
    void closeCurrent();
    UINT32 fillAudio(float* dst, UINT32 maxFrames);
    void computeNextIndex();
    void requestTrackChange();
    void wakeDecoder();
    void rebuildTempoGraph();   // 变速变化时重建 atempo 滤镜图
    void applyEq(float* data, size_t samples);  // f32 交错, 就地滤波

    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void reset() { x1 = x2 = y1 = y2 = 0; }
    };

    // 解码(ffmpeg)
    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swr_ = nullptr;
    int streamIndex_ = -1;
    int64_t outSampleRate_ = 44100;
    int outChannels_ = 2;
    double duration_ = 0.0;
    uint64_t decodedSamples_ = 0;   // 已解码输出采样计数(位置跟踪)

    // 变速滤镜图(atempo, 级联两段支持 0.25x-4x)
    AVFilterGraph* filterGraph_ = nullptr;
    AVFilterContext* filterSrc_ = nullptr;
    AVFilterContext* filterSink_ = nullptr;
    float speed_ = 1.0f;

    // 均衡器(biquad peaking, 每声道每段)
    Biquad eqBiquads_[kEqBands][2];   // [band][channel]
    float eqGains_[kEqBands] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bool eqEnabled_ = false;

    // 当前曲目信息
    std::string codecName_;
    int sampleRateIn_ = 0;
    int bitDepth_ = 16;
    int64_t bitRate_ = 0;

    // 环形缓冲(f32 交错)
    static constexpr size_t kRingFrames = 44100 * 4;  // 4 秒 @44.1k
    std::vector<float> ring_{kRingFrames * 2};
    size_t readPos_ = 0;
    size_t writePos_ = 0;
    size_t bufferedFrames_ = 0;
    mutable std::mutex ringMutex_;
    std::condition_variable ringCv_;

    std::thread decoderThread_;
    std::atomic<bool> decoderRunning_{false};
    std::atomic<bool> eof_{false};

    // 控制状态
    mutable std::mutex ctlMutex_;
    std::vector<std::string> playlist_;
    size_t index_ = 0;
    State state_ = State::Stopped;
    Mode mode_ = Mode::ListRepeat;
    int volumePct_ = 80;

    AudioOut audio_;
    bool audioOk_ = false;

    // 切歌/seek 请求
    std::atomic<bool> trackChanged_{false};
    std::atomic<bool> seekRequested_{false};
    double seekTarget_ = 0.0;
};
