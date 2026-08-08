// audio_out.cpp — waveOut 音频输出实现
#include "audio_out.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winmm.lib")

namespace {
constexpr DWORD kBufMs = 100;  // 每缓冲 100ms
}

AudioOut::AudioOut() = default;
AudioOut::~AudioOut() { shutdown(); }

bool AudioOut::init(UINT32 sampleRate, WORD channels) {
    shutdown();

    sampleRate_ = sampleRate;
    channels_ = channels;

    fmt_.wFormatTag = WAVE_FORMAT_PCM;
    fmt_.nChannels = channels_;
    fmt_.nSamplesPerSec = sampleRate_;
    fmt_.nAvgBytesPerSec = sampleRate_ * channels_ * 2;
    fmt_.nBlockAlign = static_cast<WORD>(channels_ * 2);
    fmt_.wBitsPerSample = 16;
    fmt_.cbSize = 0;

    MMRESULT mr = waveOutOpen(&hwo_, WAVE_MAPPER, &fmt_, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] waveOutOpen failed: 0x%08lX\n", (unsigned long)mr);
        hwo_ = nullptr;
        return false;
    }

    // 预分配缓冲
    framesPerBuf_ = sampleRate_ * kBufMs / 1000;
    bufs_.resize(kNumBuffers);
    for (auto& b : bufs_) {
        b.assign(framesPerBuf_ * channels_, 0);
    }
    tmpF32_.assign(framesPerBuf_ * channels_, 0.0f);
    for (auto& h : hdrs_) {
        std::memset(&h, 0, sizeof(h));
    }
    nextSubmit_ = 0;
    return true;
}

void AudioOut::shutdown() {
    stop();
    if (hwo_) {
        waveOutReset(hwo_);
        // 释放所有 hdr
        for (size_t i = 0; i < hdrs_.size(); ++i) {
            if (hdrs_[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(hwo_, &hdrs_[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(hwo_);
        hwo_ = nullptr;
    }
}

bool AudioOut::start(FillFn fill) {
    if (!hwo_ || running_.load()) return false;
    fill_ = std::move(fill);

    running_ = true;
    thread_ = std::thread([this] { threadLoop(); });
    return true;
}

void AudioOut::stop() {
    if (running_.exchange(false)) {
        if (hwo_) waveOutReset(hwo_);
        if (thread_.joinable()) thread_.join();
    }
    fill_ = nullptr;
}

void AudioOut::submitBuffer(size_t idx) {
    WAVEHDR& h = hdrs_[idx];
    short* dst = bufs_[idx].data();
    UINT32 frames = 0;
    if (fill_) {
        frames = fill_(reinterpret_cast<float*>(tmpF32_.data()), static_cast<UINT32>(framesPerBuf_));
    }
    // f32 -> s16
    size_t samples = static_cast<size_t>(frames) * channels_;
    for (size_t i = 0; i < samples; ++i) {
        float v = tmpF32_[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        dst[i] = static_cast<short>(v * 32767.0f);
    }
    // 未填满部分静音(已初始化为 0)

    h.lpData = reinterpret_cast<LPSTR>(dst);
    h.dwBufferLength = static_cast<DWORD>(frames * channels_ * sizeof(short));
    h.dwFlags = 0;
    h.dwUser = static_cast<DWORD_PTR>(idx);

    MMRESULT mr = waveOutPrepareHeader(hwo_, &h, sizeof(WAVEHDR));
    if (mr != MMSYSERR_NOERROR) return;
    mr = waveOutWrite(hwo_, &h, sizeof(WAVEHDR));
    if (mr != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(hwo_, &h, sizeof(WAVEHDR));
    }
}

void AudioOut::threadLoop() {
    // 预填前 2 个缓冲
    submitBuffer(0);
    submitBuffer(1);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 找出已播放完的缓冲并重新提交
        for (size_t i = 0; i < kNumBuffers; ++i) {
            WAVEHDR& h = hdrs_[i];
            if ((h.dwFlags & WHDR_DONE) && (h.dwFlags & WHDR_PREPARED)) {
                waveOutUnprepareHeader(hwo_, &h, sizeof(WAVEHDR));
                submitBuffer(i);
            }
        }
    }

    // 停止: 重置
    if (hwo_) waveOutReset(hwo_);
}
