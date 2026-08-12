// cover.cpp — 专辑封面: FFmpeg 提取内嵌封面 + 解码 + 像素模糊色块渲染
#include "cover.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// half-block (chafa/fastfetch 默认): 每字符格 1 列 x 2 行像素, 实心无缝
constexpr int kCellW = 1;
constexpr int kCellH = 2;
constexpr int kMaxDim = 2048;  // 解码防炸弹上限

// ---------- 1. 从音频文件提取内嵌封面 (attached_pic) ----------
std::vector<uint8_t> extractCover(const std::string& path) {
    std::vector<uint8_t> out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return out; }

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        AVPacket* pkt = &st->attached_pic;
        if (!pkt || pkt->size <= 0) continue;
        out.assign(pkt->data, pkt->data + pkt->size);
        break;
    }
    avformat_close_input(&fmt);
    return out;
}

// ---------- 2. 解码图片字节为 RGB888 ----------
// 成功返回 true, rgb 填满 w*h*3
bool decodeToRgb(const uint8_t* data, size_t size, int& w, int& h,
                 std::vector<uint8_t>& rgb) {
    if (!data || size == 0) return false;

    AVCodecID guess = AV_CODEC_ID_NONE;
    if (size > 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        guess = AV_CODEC_ID_PNG;
    } else if (size > 2 && data[0] == 0xFF && data[1] == 0xD8) {
        guess = AV_CODEC_ID_MJPEG;
    } else if (size > 2 && data[0] == 'B' && data[1] == 'M') {
        guess = AV_CODEC_ID_BMP;
    }
    const AVCodec* codec = guess != AV_CODEC_ID_NONE
        ? avcodec_find_decoder(guess)
        : avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    bool ok = false;
    if (frame && pkt) {
        pkt->data = const_cast<uint8_t*>(data);
        pkt->size = static_cast<int>(size);
        if (avcodec_open2(ctx, codec, nullptr) == 0 &&
            avcodec_send_packet(ctx, pkt) == 0 &&
            avcodec_receive_frame(ctx, frame) == 0) {
            w = frame->width;
            h = frame->height;
            if (w > 0 && h > 0 && w <= kMaxDim && h <= kMaxDim) {
                // 统一转为 RGB888 (不依赖 swscale, 裁剪版禁用):
                // YUV420/422/444 取 Y 平面 + 按 4:2:0/4:2:2/4:4:4 插值 UV;
                // RGB24/BGR24 直接搬运; GRAY8 复制为灰。
                const AVPixelFormat fmt = (AVPixelFormat)frame->format;
                rgb.resize(static_cast<size_t>(w) * h * 3);
                auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                    size_t o = (static_cast<size_t>(y) * w + x) * 3;
                    rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = b;
                };
                switch (fmt) {
                    case AV_PIX_FMT_YUV420P:
                    case AV_PIX_FMT_YUVJ420P: {
                        for (int y = 0; y < h; ++y) {
                            for (int x = 0; x < w; ++x) {
                                uint8_t Y = frame->data[0][static_cast<size_t>(y) * frame->linesize[0] + x];
                                uint8_t U = frame->data[1][static_cast<size_t>(y / 2) * frame->linesize[1] + x / 2];
                                uint8_t V = frame->data[2][static_cast<size_t>(y / 2) * frame->linesize[2] + x / 2];
                                int c = static_cast<int>(Y) - 16, d = static_cast<int>(U) - 128, e = static_cast<int>(V) - 128;
                                put(x, y,
                                    static_cast<uint8_t>(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255)));
                            }
                        }
                        ok = true;
                        break;
                    }
                    case AV_PIX_FMT_YUV422P:
                    case AV_PIX_FMT_YUVJ422P: {
                        for (int y = 0; y < h; ++y) {
                            for (int x = 0; x < w; ++x) {
                                uint8_t Y = frame->data[0][static_cast<size_t>(y) * frame->linesize[0] + x];
                                uint8_t U = frame->data[1][static_cast<size_t>(y) * frame->linesize[1] + x / 2];
                                uint8_t V = frame->data[2][static_cast<size_t>(y) * frame->linesize[2] + x / 2];
                                int c = static_cast<int>(Y) - 16, d = static_cast<int>(U) - 128, e = static_cast<int>(V) - 128;
                                put(x, y,
                                    static_cast<uint8_t>(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255)));
                            }
                        }
                        ok = true;
                        break;
                    }
                    case AV_PIX_FMT_YUV444P:
                    case AV_PIX_FMT_YUVJ444P: {
                        for (int y = 0; y < h; ++y) {
                            for (int x = 0; x < w; ++x) {
                                uint8_t Y = frame->data[0][static_cast<size_t>(y) * frame->linesize[0] + x];
                                uint8_t U = frame->data[1][static_cast<size_t>(y) * frame->linesize[1] + x];
                                uint8_t V = frame->data[2][static_cast<size_t>(y) * frame->linesize[2] + x];
                                int c = static_cast<int>(Y) - 16, d = static_cast<int>(U) - 128, e = static_cast<int>(V) - 128;
                                put(x, y,
                                    static_cast<uint8_t>(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255)),
                                    static_cast<uint8_t>(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255)));
                            }
                        }
                        ok = true;
                        break;
                    }
                    case AV_PIX_FMT_RGB24:
                    case AV_PIX_FMT_BGR24: {
                        for (int y = 0; y < h; ++y) {
                            const uint8_t* row = frame->data[0] + static_cast<size_t>(y) * frame->linesize[0];
                            for (int x = 0; x < w; ++x) {
                                const uint8_t* p = row + static_cast<size_t>(x) * 3;
                                if (fmt == AV_PIX_FMT_RGB24) put(x, y, p[0], p[1], p[2]);
                                else put(x, y, p[2], p[1], p[0]);
                            }
                        }
                        ok = true;
                        break;
                    }
                    default:
                        // 其它(含 GRAY8): 取 data[0] 作为亮度, 复制为灰
                        if (frame->linesize[0] >= w) {
                            for (int y = 0; y < h; ++y) {
                                const uint8_t* row = frame->data[0] + static_cast<size_t>(y) * frame->linesize[0];
                                for (int x = 0; x < w; ++x) put(x, y, row[x], row[x], row[x]);
                            }
                            ok = true;
                        }
                        break;
                }
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

// ---------- 3. Lanczos-3 重采样 RGB (到 cols x rows*2 像素) ----------
// 比双线性更锐利, 保留更多高频细节(清晰度优先; 显示尺寸由字符格数决定)
static float lanczosKernel(float x) {
    if (x <= -3.0f || x >= 3.0f) return 0.0f;
    if (x == 0.0f) return 1.0f;
    float px = 3.14159265f * x;
    float s = 3.14159265f * x / 3.0f;
    return (3.0f * std::sin(px) * std::sin(s)) / (px * s);
}

void resizeRgb(const uint8_t* src, int sw, int sh,
               uint8_t* dst, int dw, int dh) {
    constexpr int kTap = 3;
    float sx = static_cast<float>(sw) / dw;
    float sy = static_cast<float>(sh) / dh;
    // 预计算水平权重
    std::vector<float> wx(static_cast<size_t>(dw) * (2 * kTap + 1));
    std::vector<int> ix(static_cast<size_t>(dw) * (2 * kTap + 1));
    for (int dx = 0; dx < dw; ++dx) {
        float fx = (dx + 0.5f) * sx - 0.5f;
        int cx = static_cast<int>(std::floor(fx));
        float sum = 0.0f;
        for (int t = -kTap; t <= kTap; ++t) {
            int idx = cx + t;
            float w = (idx >= 0 && idx < sw) ? lanczosKernel(fx - idx) : 0.0f;
            wx[static_cast<size_t>(dx) * (2 * kTap + 1) + t + kTap] = w;
            ix[static_cast<size_t>(dx) * (2 * kTap + 1) + t + kTap] = idx;
            sum += w;
        }
        if (sum > 0.0f) {
            for (int t = 0; t < 2 * kTap + 1; ++t)
                wx[static_cast<size_t>(dx) * (2 * kTap + 1) + t] /= sum;
        }
    }
    // 临时水平中间缓冲
    std::vector<uint8_t> tmp(static_cast<size_t>(sh) * dw * 3);
    for (int y = 0; y < sh; ++y) {
        const uint8_t* srow = src + static_cast<size_t>(y) * sw * 3;
        for (int dx = 0; dx < dw; ++dx) {
            size_t wbase = static_cast<size_t>(dx) * (2 * kTap + 1);
            float acc[3] = {0, 0, 0};
            for (int t = 0; t < 2 * kTap + 1; ++t) {
                int idx = ix[wbase + t];
                if (idx < 0 || idx >= sw) continue;
                float w = wx[wbase + t];
                const uint8_t* p = srow + static_cast<size_t>(idx) * 3;
                acc[0] += w * p[0]; acc[1] += w * p[1]; acc[2] += w * p[2];
            }
            size_t o = (static_cast<size_t>(y) * dw + dx) * 3;
            tmp[o] = static_cast<uint8_t>(acc[0] + 0.5f);
            tmp[o + 1] = static_cast<uint8_t>(acc[1] + 0.5f);
            tmp[o + 2] = static_cast<uint8_t>(acc[2] + 0.5f);
        }
    }
    // 垂直方向: 逐目标行 Lanczos
    for (int dy = 0; dy < dh; ++dy) {
        float fy = (dy + 0.5f) * sy - 0.5f;
        int cy = static_cast<int>(std::floor(fy));
        float wy[2 * kTap + 1];
        float wsum = 0.0f;
        for (int t = -kTap; t <= kTap; ++t) {
            int idx = cy + t;
            wy[t + kTap] = (idx >= 0 && idx < sh) ? lanczosKernel(fy - idx) : 0.0f;
            wsum += wy[t + kTap];
        }
        if (wsum > 0.0f)
            for (auto& w : wy) w /= wsum;
        for (int dx = 0; dx < dw; ++dx) {
            float acc[3] = {0, 0, 0};
            for (int t = -kTap; t <= kTap; ++t) {
                int idx = cy + t;
                if (idx < 0 || idx >= sh) continue;
                const uint8_t* p = tmp.data() + (static_cast<size_t>(idx) * dw + dx) * 3;
                float w = wy[t + kTap];
                acc[0] += w * p[0]; acc[1] += w * p[1]; acc[2] += w * p[2];
            }
            size_t o = (static_cast<size_t>(dy) * dw + dx) * 3;
            dst[o] = static_cast<uint8_t>(acc[0] + 0.5f);
            dst[o + 1] = static_cast<uint8_t>(acc[1] + 0.5f);
            dst[o + 2] = static_cast<uint8_t>(acc[2] + 0.5f);
        }
    }
}

// ---------- 4. Floyd-Steinberg 误差扩散抖动 ----------
// chafa/fastfetch 默认启用: 量化到目标色域时的误差扩散到相邻像素,
// 让低分辨率色块边缘平滑渐变(着色器观感)。这里以 16 级/通道量化模拟。
void ditherFloydSteinberg(uint8_t* rgb, int w, int h) {
    constexpr int levels = 16;  // 每通道 16 级
    constexpr int step = 255 / (levels - 1);
    std::vector<int> err(static_cast<size_t>(w) * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t o = (static_cast<size_t>(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c) {
                int v = rgb[o + c] + err[o + c];
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                int quant = (v + step / 2) / step * step;
                if (quant > 255) quant = 255;
                rgb[o + c] = static_cast<uint8_t>(quant);
                int e = v - quant;
                // FS 扩散: 7/16 右, 3/16 左下, 5/16 下, 1/16 右下
                if (x + 1 < w)           err[o + 3 + c] += e * 7 / 16;
                if (y + 1 < h) {
                    if (x > 0)            err[o + static_cast<size_t>(w) * 3 - 3 + c] += e * 3 / 16;
                                          err[o + static_cast<size_t>(w) * 3 + c] += e * 5 / 16;
                    if (x + 1 < w)        err[o + static_cast<size_t>(w) * 3 + 3 + c] += e * 1 / 16;
                }
            }
        }
    }
}

}  // namespace

// ==================== 公开接口 ====================
CoverImage coverDecode(const std::string& audioPath) {
    CoverImage img;
    if (audioPath.empty()) return img;

    // 1. 提取内嵌封面
    std::vector<uint8_t> data = extractCover(audioPath);
    if (data.empty()) return img;

    // 2. 解码为 RGB888
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!decodeToRgb(data.data(), data.size(), w, h, rgb)) return img;

    img.rgb = std::move(rgb);
    img.w = w;
    img.h = h;
    return img;
}

std::vector<CoverCell> coverCellsFromRgb(const CoverImage& img,
                                         int targetCols, int targetRows) {
    std::vector<CoverCell> cells;
    if (!img.valid() || targetCols <= 0 || targetRows <= 0) return cells;

    // 3. Lanczos 缩放到 cols x (rows*2) 像素 (half-block 每字符格 = 2 行像素)
    int pw = targetCols * kCellW, ph = targetRows * kCellH;
    std::vector<uint8_t> scaled(static_cast<size_t>(pw) * ph * 3);
    resizeRgb(img.rgb.data(), img.w, img.h, scaled.data(), pw, ph);

    // 4. 真彩色直接输出(终端 24-bit, 无需量化抖动)。
    //    双线性缩放本身已平滑; 量化会引入色带发糊, 故省略。
    //    原 Floyd-Steinberg 抖动(16 级量化)仅为色域受限终端所需。

    // 5. 每字符格取上下两个像素 -> CoverCell (上半=前景色, 下半=背景色)
    cells.reserve(static_cast<size_t>(targetRows) * targetCols);
    for (int row = 0; row < targetRows; ++row) {
        for (int col = 0; col < targetCols; ++col) {
            CoverCell c;
            size_t top = (static_cast<size_t>(row * 2) * pw + col) * 3;
            size_t bot = (static_cast<size_t>(row * 2 + 1) * pw + col) * 3;
            c.r1 = scaled[top]; c.g1 = scaled[top + 1]; c.b1 = scaled[top + 2];
            c.r2 = scaled[bot]; c.g2 = scaled[bot + 1]; c.b2 = scaled[bot + 2];
            cells.push_back(c);
        }
    }
    return cells;
}
