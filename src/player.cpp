// player.cpp — FFmpeg 解码 + 环形缓冲 + WASAPI 输出
#include "player.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace {
std::mutex g_ffmpegInitMutex;
bool g_ffmpegInitDone = false;

void ensureFfmpegInit() {
    std::lock_guard<std::mutex> lock(g_ffmpegInitMutex);
    if (!g_ffmpegInitDone) {
        avformat_network_init();
        g_ffmpegInitDone = true;
    }
}
}  // namespace

Player::Player() {
    ensureFfmpegInit();
    ring_.assign(kRingFrames * 2, 0.0f);
}

Player::~Player() {
    decoderRunning_ = false;
    if (decoderThread_.joinable()) decoderThread_.join();
    closeCurrent();
    audio_.shutdown();
}

// ---- 变速 ----

void Player::setSpeed(float speed) {
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        speed_ = speed;
    }
    rebuildTempoGraph();
}

float Player::speed() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return speed_;
}

void Player::rebuildTempoGraph() {
    // 仅在解码线程内调用(openCurrent / seek / setSpeed 时)
    if (filterGraph_) {
        avfilter_graph_free(&filterGraph_);
        filterGraph_ = nullptr;
        filterSrc_ = filterSink_ = nullptr;
    }
    if (!fmtCtx_) return;

    filterGraph_ = avfilter_graph_alloc();
    if (!filterGraph_) return;

    const AVFilter* src = avfilter_get_by_name("abuffer");
    const AVFilter* sink = avfilter_get_by_name("abuffersink");
    if (!src || !sink) {
        avfilter_graph_free(&filterGraph_);
        filterGraph_ = nullptr;
        return;
    }

    // FFmpeg 7: 用新 channel layout API 取掩码
    AVChannelLayout chl;
    av_channel_layout_default(&chl, outChannels_);
    int64_t chMask = (chl.order == AV_CHANNEL_ORDER_NATIVE) ? chl.u.mask : 0x3;
    av_channel_layout_uninit(&chl);

    // 级联两段 atempo 参数(sqrt 分解, 支持 0.25x-4x 全程保调)
    float s1 = 1.0f, s2 = 1.0f;
    {
        float sp = speed_;
        if (sp < 0.25f) sp = 0.25f;
        if (sp > 4.0f) sp = 4.0f;
        s1 = sqrtf(sp);
        s2 = sp / s1;
    }

    char args[256];
    snprintf(args, sizeof(args),
             "time_base=1/%lld:sample_rate=%lld:sample_fmt=flt:channel_layout=0x%llx",
             (long long)outSampleRate_, (long long)outSampleRate_,
             (unsigned long long)chMask);

    AVFilterContext* prev = nullptr;
    AVFilterContext* tmp = nullptr;
    char opt[32] = {0};

    if (avfilter_graph_create_filter(&filterSrc_, src, "in", args, nullptr, filterGraph_) < 0) {
        goto fail;
    }
    if (avfilter_graph_create_filter(&filterSink_, sink, "out", nullptr, nullptr, filterGraph_) < 0) {
        goto fail;
    }

    prev = filterSrc_;

    snprintf(opt, sizeof(opt), "%f", s1);
    if (avfilter_graph_create_filter(&tmp, avfilter_get_by_name("atempo"), "t1",
                                     opt, nullptr, filterGraph_) < 0) {
        goto fail;
    }
    if (avfilter_link(prev, 0, tmp, 0) < 0) goto fail;
    prev = tmp;

    snprintf(opt, sizeof(opt), "%f", s2);
    if (avfilter_graph_create_filter(&tmp, avfilter_get_by_name("atempo"), "t2",
                                     opt, nullptr, filterGraph_) < 0) {
        goto fail;
    }
    if (avfilter_link(prev, 0, tmp, 0) < 0) goto fail;
    prev = tmp;

    if (avfilter_link(prev, 0, filterSink_, 0) < 0) goto fail;
    if (avfilter_graph_config(filterGraph_, nullptr) < 0) goto fail;
    return;

fail:
    avfilter_graph_free(&filterGraph_);
    filterGraph_ = nullptr;
    filterSrc_ = filterSink_ = nullptr;
}

// ---- 均衡器 ----

void Player::setEqBand(int band, float db) {
    if (band < 0 || band >= kEqBands) return;
    if (db < -12.0f) db = -12.0f;
    if (db > 12.0f) db = 12.0f;
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        eqGains_[band] = db;
    }
    // RBJ peaking EQ 系数
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * 3.14159265358979f * kEqFreqs[band] / static_cast<float>(outSampleRate_);
    float alpha = sinf(w0) / 2.0f;  // Q = 1
    for (int ch = 0; ch < 2; ++ch) {
        Biquad& q = eqBiquads_[band][ch];
        q.b0 = 1.0f + alpha * A;
        q.b1 = -2.0f * cosf(w0);
        q.b2 = 1.0f - alpha * A;
        q.a1 = -2.0f * cosf(w0);
        q.a2 = 1.0f - alpha / A;
        float a0 = 1.0f + alpha / A;
        q.b0 /= a0; q.b1 /= a0; q.b2 /= a0;
        q.a1 /= a0; q.a2 /= a0;
        q.reset();
    }
}

float Player::eqBand(int band) const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    if (band < 0 || band >= kEqBands) return 0.0f;
    return eqGains_[band];
}

void Player::setEqEnabled(bool on) {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    eqEnabled_ = on;
}

bool Player::eqEnabled() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return eqEnabled_;
}

void Player::setEqPreset(const float* gainsDb) {
    for (int b = 0; b < kEqBands; ++b) {
        setEqBand(b, gainsDb[b]);
    }
}

void Player::applyEq(float* data, size_t samples) {
    if (!eqEnabled_) return;
    for (size_t i = 0; i < samples; ++i) {
        float* p = data + i * static_cast<size_t>(outChannels_);
        for (int ch = 0; ch < outChannels_ && ch < 2; ++ch) {
            float cur = p[ch];
            for (int b = 0; b < kEqBands; ++b) {
                Biquad& q = eqBiquads_[b][ch];
                float yn = q.b0 * cur + q.b1 * q.x1 + q.b2 * q.x2 - q.a1 * q.y1 - q.a2 * q.y2;
                q.x2 = q.x1; q.x1 = cur;
                q.y2 = q.y1; q.y1 = yn;
                cur = yn;
            }
            p[ch] = cur;
        }
    }
}

// ---- 信息 ----

std::string Player::codecName() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return codecName_;
}

int Player::sampleRate() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return sampleRateIn_;
}

int Player::bitDepth() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return bitDepth_;
}

int64_t Player::bitRate() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return bitRate_;
}

bool Player::initOutput() {
    audioOk_ = audio_.init(44100, 2);
    if (audioOk_) {
        outSampleRate_ = audio_.sampleRate();
        outChannels_ = audio_.channels();
    } else {
        outSampleRate_ = 44100;
        outChannels_ = 2;
    }
    return audioOk_;
}

void Player::setPlaylist(std::vector<std::string> files) {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    playlist_ = std::move(files);
    if (index_ >= playlist_.size()) index_ = 0;
}

void Player::play(size_t index) {
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        if (playlist_.empty()) return;
        if (index < playlist_.size()) index_ = index;
        state_ = State::Playing;
    }
    if (!decoderRunning_.load()) {
        decoderRunning_ = true;
        decoderThread_ = std::thread([this] { decoderLoop(); });
    } else {
        requestTrackChange();
        wakeDecoder();
    }
    if (!audio_.start([this](float* dst, UINT32 n) { return fillAudio(dst, n); })) {
        // WASAPI 不可用时仍保持状态机(静默播放)
    }
}

void Player::toggle() {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    if (state_ == State::Playing) {
        state_ = State::Paused;
    } else if (state_ == State::Paused || state_ == State::Stopped) {
        state_ = State::Playing;
    }
    wakeDecoder();
}

void Player::pause() {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    if (state_ == State::Playing) state_ = State::Paused;
    wakeDecoder();
}

void Player::resume() {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    if (state_ == State::Paused || state_ == State::Stopped) state_ = State::Playing;
    wakeDecoder();
}

void Player::stop() {
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        state_ = State::Stopped;
    }
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        readPos_ = writePos_ = bufferedFrames_ = 0;
    }
    eof_ = false;
    seekRequested_ = false;
    wakeDecoder();
}

void Player::seek(double seconds) {
    if (seconds < 0) seconds = 0;
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        readPos_ = writePos_ = bufferedFrames_ = 0;
    }
    seekTarget_ = seconds;
    seekRequested_ = true;
    wakeDecoder();
}

void Player::next() {
    computeNextIndex();
    requestTrackChange();
    wakeDecoder();
}

void Player::prev() {
    // 播放超过 3 秒时回到开头, 否则上一首
    if (position() > 3.0) {
        seek(0.0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        if (playlist_.size() > 1) {
            index_ = (index_ + playlist_.size() - 1) % playlist_.size();
        }
    }
    requestTrackChange();
    wakeDecoder();
}

void Player::cycleMode() {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    mode_ = static_cast<Mode>((static_cast<int>(mode_) + 1) % 5);
}

Player::Mode Player::mode() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return mode_;
}

const char* Player::modeName() const {
    switch (mode()) {
        case Mode::Sequential:   return "顺序播放";
        case Mode::ListRepeat:   return "列表循环";
        case Mode::SingleRepeat: return "单曲循环";
        case Mode::Shuffle:      return "随机播放";
        case Mode::ShuffleOnce:  return "随机单次";
    }
    return "?";
}

void Player::setVolume(int pct) {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    volumePct_ = std::clamp(pct, 0, 100);
}

int Player::volume() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return volumePct_;
}

Player::State Player::state() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return state_;
}

size_t Player::currentIndex() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    return index_;
}

double Player::position() const {
    int64_t rate = outSampleRate_;
    float sp = speed_;
    uint64_t samples = decodedSamples_;
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        // 已解码位置减去缓冲中未播放的部分 = 实际播放位置
        uint64_t played = samples > bufferedFrames_ ? samples - bufferedFrames_ : 0;
        // 输出采样换算回输入时间(变速时进度按歌曲时间走)
        return static_cast<double>(played) * static_cast<double>(sp) /
               static_cast<double>(rate);
    }
}

double Player::duration() const {
    return duration_;
}

std::string Player::currentTitle() const {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    if (index_ >= playlist_.size()) return "";
    return std::filesystem::path(playlist_[index_]).filename().string();
}

const std::vector<std::string>& Player::playlist() const {
    return playlist_;
}

// ---- 内部 ----

void Player::wakeDecoder() {
    ringCv_.notify_all();
}

void Player::requestTrackChange() {
    trackChanged_ = true;
    {
        std::lock_guard<std::mutex> lock(ringMutex_);
        readPos_ = writePos_ = bufferedFrames_ = 0;
    }
    eof_ = false;
}

void Player::computeNextIndex() {
    std::lock_guard<std::mutex> lock(ctlMutex_);
    size_t n = playlist_.size();
    if (n == 0) return;
    switch (mode_) {
        case Mode::Shuffle:
        case Mode::ShuffleOnce:
            index_ = static_cast<size_t>(rand()) % n;
            break;
        case Mode::SingleRepeat:
            break;  // 同一首
        default:
            index_ = (index_ + 1) % n;
            break;
    }
}

bool Player::openCurrent() {
    closeCurrent();

    std::string path;
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        if (index_ >= playlist_.size()) return false;
        path = playlist_[index_];
    }

    if (avformat_open_input(&fmtCtx_, path.c_str(), nullptr, nullptr) != 0) {
        return false;
    }
    if (avformat_find_stream_info(fmtCtx_, nullptr) < 0) {
        return false;
    }
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        if (fmtCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            streamIndex_ = static_cast<int>(i);
            break;
        }
    }
    if (streamIndex_ < 0) return false;

    AVStream* stream = fmtCtx_->streams[streamIndex_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return false;
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return false;
    if (avcodec_parameters_to_context(codecCtx_, stream->codecpar) < 0) return false;
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) return false;

    duration_ = static_cast<double>(fmtCtx_->duration) / AV_TIME_BASE;
    if (duration_ <= 0 && stream->duration > 0) {
        duration_ = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    // 信息栏数据
    {
        std::lock_guard<std::mutex> lock(ctlMutex_);
        codecName_ = codec->name ? codec->name : "?";
        sampleRateIn_ = codecCtx_->sample_rate;
        bitDepth_ = av_get_bytes_per_sample(codecCtx_->sample_fmt) * 8;
        bitRate_ = codecCtx_->bit_rate > 0 ? codecCtx_->bit_rate : fmtCtx_->bit_rate;
        if (bitRate_ <= 0) bitRate_ = 0;
    }

    swr_ = swr_alloc();
    if (!swr_) return false;
    av_opt_set_chlayout(swr_, "in_chlayout", &codecCtx_->ch_layout, 0);
    av_opt_set_int(swr_, "in_sample_rate", codecCtx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", codecCtx_->sample_fmt, 0);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels_);
    av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
    av_channel_layout_uninit(&outLayout);
    av_opt_set_int(swr_, "out_sample_rate", outSampleRate_, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    if (swr_init(swr_) < 0) return false;

    rebuildTempoGraph();
    decodedSamples_ = 0;
    return true;
}

void Player::closeCurrent() {
    if (filterGraph_) {
        avfilter_graph_free(&filterGraph_);
        filterGraph_ = nullptr;
        filterSrc_ = filterSink_ = nullptr;
    }
    if (swr_) { swr_free(&swr_); swr_ = nullptr; }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (fmtCtx_) { avformat_close_input(&fmtCtx_); fmtCtx_ = nullptr; }
    streamIndex_ = -1;
}

void Player::decoderLoop() {
    bool opened = false;

    while (decoderRunning_.load()) {
        // 等待播放状态
        {
            std::unique_lock<std::mutex> lock(ctlMutex_);
            ringCv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return !decoderRunning_.load() || state_ != State::Paused ||
                       trackChanged_.load() || seekRequested_.load();
            });
        }
        if (!decoderRunning_.load()) break;

        State st;
        {
            std::lock_guard<std::mutex> lock(ctlMutex_);
            st = state_;
        }

        // 切歌请求
        if (trackChanged_.exchange(false)) {
            closeCurrent();
            opened = openCurrent();
            if (!opened) {
                eof_ = true;
                continue;
            }
        }
        if (!opened) {
            opened = openCurrent();
            if (!opened) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }

        // seek 请求
        if (seekRequested_.exchange(false)) {
            double target = seekTarget_;
            int64_t ts = static_cast<int64_t>(target * AV_TIME_BASE);
            av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codecCtx_);
            if (swr_) swr_convert(swr_, nullptr, 0, nullptr, 0);  // 丢弃残留
            rebuildTempoGraph();  // 重建滤镜图, 清 atempo 内部状态
            float sp;
            {
                std::lock_guard<std::mutex> lock(ctlMutex_);
                sp = speed_;
            }
            // decodedSamples_ 记输出采样: position = played * speed / rate = target
            decodedSamples_ = static_cast<uint64_t>(target * outSampleRate_ / (sp > 0.01f ? sp : 1.0f));
            eof_ = false;
        }

        if (st != State::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 环形缓冲有空间才解码
        {
            std::unique_lock<std::mutex> lock(ringMutex_);
            if (bufferedFrames_ >= kRingFrames - 4096) {
                ringCv_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
        }

        AVPacket pkt;
        int readRet = av_read_frame(fmtCtx_, &pkt);
        if (readRet < 0) {
            av_packet_unref(&pkt);
            if (readRet == AVERROR_EOF) {
                // 当前文件结束: 按模式处理
                Mode m;
                {
                    std::lock_guard<std::mutex> lock(ctlMutex_);
                    m = mode_;
                    if (m != Mode::SingleRepeat && !playlist_.empty()) {
                        if (m == Mode::Shuffle || m == Mode::ShuffleOnce) {
                            index_ = static_cast<size_t>(rand()) % playlist_.size();
                        } else if (m == Mode::Sequential && index_ + 1 >= playlist_.size()) {
                            // 顺序播放到列表尾: 停止
                            state_ = State::Stopped;
                            closeCurrent();
                            opened = false;
                            continue;
                        } else {
                            index_ = (index_ + 1) % playlist_.size();
                        }
                    }
                }
                trackChanged_ = true;
                eof_ = false;
                {
                    std::lock_guard<std::mutex> lock(ringMutex_);
                    readPos_ = writePos_ = bufferedFrames_ = 0;
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (pkt.stream_index != streamIndex_) {
            av_packet_unref(&pkt);
            continue;
        }
        if (avcodec_send_packet(codecCtx_, &pkt) < 0) {
            av_packet_unref(&pkt);
            continue;
        }
        av_packet_unref(&pkt);

        AVFrame* frame = av_frame_alloc();
        while (avcodec_receive_frame(codecCtx_, frame) == 0) {
            // swr 到 f32 交错
            int outSamples = swr_get_out_samples(swr_, frame->nb_samples);
            std::vector<float> buf(static_cast<size_t>(outSamples) * outChannels_);
            uint8_t* dstPlanes[1] = {reinterpret_cast<uint8_t*>(buf.data())};
            int converted = swr_convert(swr_, dstPlanes, outSamples,
                                        const_cast<const uint8_t**>(frame->extended_data),
                                        frame->nb_samples);
            if (converted > 0) {
                bool handled = false;
                // 变速滤镜(atempo)
                if (filterSrc_ && filterSink_) {
                    AVFrame* fin = av_frame_alloc();
                    fin->format = AV_SAMPLE_FMT_FLT;
                    fin->nb_samples = converted;
                    fin->sample_rate = static_cast<int>(outSampleRate_);
                    av_channel_layout_default(&fin->ch_layout, outChannels_);
                    fin->data[0] = reinterpret_cast<uint8_t*>(buf.data());
                    fin->linesize[0] = converted * outChannels_ * 4;
                    int sret = av_buffersrc_add_frame(filterSrc_, fin);
                    av_frame_free(&fin);
                    if (sret >= 0) {
                        AVFrame* fout = av_frame_alloc();
                        while (av_buffersink_get_frame(filterSink_, fout) == 0) {
                            float* fdata = reinterpret_cast<float*>(fout->data[0]);
                            size_t fn = static_cast<size_t>(fout->nb_samples);
                            applyEq(fdata, fn);
                            {
                                std::lock_guard<std::mutex> lock(ringMutex_);
                                size_t space = kRingFrames - bufferedFrames_;
                                size_t toCopy = std::min(fn, space);
                                if (toCopy > 0) {
                                    size_t first = std::min(toCopy, kRingFrames - writePos_);
                                    std::memcpy(ring_.data() + writePos_ * outChannels_,
                                                fdata, first * outChannels_ * sizeof(float));
                                    if (toCopy > first) {
                                        std::memcpy(ring_.data(),
                                                    fdata + first * outChannels_,
                                                    (toCopy - first) * outChannels_ * sizeof(float));
                                    }
                                    writePos_ = (writePos_ + toCopy) % kRingFrames;
                                    bufferedFrames_ += toCopy;
                                    decodedSamples_ += toCopy;
                                }
                            }
                            av_frame_unref(fout);
                        }
                        av_frame_free(&fout);
                        handled = true;
                    }
                }
                if (!handled) {
                    // 无滤镜(回退): 直接写 ring
                    std::lock_guard<std::mutex> lock(ringMutex_);
                    size_t frames = static_cast<size_t>(converted);
                    size_t space = kRingFrames - bufferedFrames_;
                    size_t toCopy = std::min(frames, space);
                    if (toCopy > 0) {
                        size_t first = std::min(toCopy, kRingFrames - writePos_);
                        std::memcpy(ring_.data() + writePos_ * outChannels_,
                                    buf.data(), first * outChannels_ * sizeof(float));
                        if (toCopy > first) {
                            std::memcpy(ring_.data(),
                                        buf.data() + first * outChannels_,
                                        (toCopy - first) * outChannels_ * sizeof(float));
                        }
                        writePos_ = (writePos_ + toCopy) % kRingFrames;
                        bufferedFrames_ += toCopy;
                        decodedSamples_ += toCopy;
                    }
                }
                ringCv_.notify_all();
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    }
}

UINT32 Player::fillAudio(float* dst, UINT32 maxFrames) {
    std::lock_guard<std::mutex> lock(ringMutex_);
    State st;
    {
        std::lock_guard<std::mutex> lock2(ctlMutex_);
        st = state_;
    }
    float gain = static_cast<float>(volumePct_) / 100.0f;

    if (st != State::Playing) {
        return 0;  // 静音
    }

    UINT32 toCopy = static_cast<UINT32>(std::min<size_t>(bufferedFrames_, maxFrames));
    if (toCopy > 0) {
        size_t first = std::min<size_t>(toCopy, kRingFrames - readPos_);
        const float* src0 = ring_.data() + readPos_ * outChannels_;
        for (size_t i = 0; i < first * outChannels_; ++i) {
            dst[i] = src0[i] * gain;
        }
        if (toCopy > first) {
            const float* src1 = ring_.data();
            size_t off = first * outChannels_;
            for (size_t i = 0; i < (toCopy - first) * outChannels_; ++i) {
                dst[off + i] = src1[i] * gain;
            }
        }
        readPos_ = (readPos_ + toCopy) % kRingFrames;
        bufferedFrames_ -= toCopy;
    }
    ringCv_.notify_all();
    return toCopy;
}
