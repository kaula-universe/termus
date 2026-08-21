// termus_bridge.cpp — termus 桥接层实现
// FTXUI UI + Player(FFmpeg 解码) + waveOut 输出的 C 封装。
// 事件通过注册的 C 函数指针回调给 Kaula 代码处理。
#include "termus_bridge.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "src/player.h"
#include "src/cover.h"

extern "C" {
#include <libavformat/avformat.h>
}

// Windows wingdi.h 定义了 RGB(r,g,b) 宏, 会污染 FTXUI 的 Color::RGB 成员
#ifdef RGB
#undef RGB
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

namespace {

Player g_player;
std::vector<std::string> g_files;
// 播放列表显示条目(随 refresh 重建; Menu 直接引用本向量, 刷新后 UI 即更新)
std::vector<std::string> g_entries;
std::vector<std::string> g_favorites;
std::vector<std::string> g_history;
std::string g_scanDir;
bool g_hasPlaylist = false;

// 睡眠定时器(T 键循环 30/60/90/关): 到点暂停, 标题栏显示倒计时
int g_sleepMin = 0;
bool g_sleepActive = false;
std::chrono::steady_clock::time_point g_sleepEnd;
std::string g_sleepMsg;
uint64_t g_sleepMsgMs = 0;   // 提示消息显示截止(epoch ms)

// 歌曲详情弹窗(i 键)
bool g_showDetail = false;

static uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() / 1000000ULL);
}

// 设置项(持久化)
bool g_autoPlay = false;
bool g_showLrcPanel = true;
int g_autoplaySel = 0;
int g_showlrcSel = 0;

// 视图: 0=主界面 1=设置
int g_viewSel = 0;

// 上次播放位置(持久化)
std::string g_lastTrack;
double g_lastPos = 0.0;

// ---- 专辑封面(切歌解码缓存 + 渲染时动态重采样, 同 C++ 版) ----
CoverImage g_coverImg;                    // 解码缓存(重操作, 切歌时做一次)
std::vector<CoverCell> g_coverCells;      // 当前尺寸的色块
int g_coverGenW = 0, g_coverGenH = 0;     // 上次生成时的终端尺寸
size_t g_coverTrackIdx = SIZE_MAX;        // 已解码的曲目

// ---- 拼音首字母(复用原版 pinyin_table.h, 同 C++ 版, 支持中文搜索) ----
#include "src/pinyin_table.h"

int Utf8Decode(const char* s, size_t len, size_t& adv) {
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) { adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && len >= 2) { adv = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        adv = 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        adv = 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    adv = 1;
    return -1;
}

char PinyinInitialCp(int cp) {
    if (cp < PINYIN_TABLE_BASE || cp >= PINYIN_TABLE_BASE + PINYIN_TABLE_SIZE) return 0;
    uint32_t offset = g_pinyin_offsets[cp - PINYIN_TABLE_BASE];
    if (offset == 0 || offset >= sizeof(g_pinyin_blob)) return 0;
    return static_cast<char>(g_pinyin_blob[offset]);
}

std::string PinyinAbbr(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t adv = 1;
        int cp = Utf8Decode(s.c_str() + i, s.size() - i, adv);
        if (cp > 0) {
            char pi = PinyinInitialCp(cp);
            if (pi) { out += pi; i += adv; continue; }
            if (cp < 128 && std::isalpha(cp)) out += static_cast<char>(std::tolower(cp));
        }
        i += adv;
    }
    return out;
}

// 切歌时调用: 解码封面缓存(轻量封装)
void CoverLoadFor(size_t idx, const std::vector<std::string>& files) {
    g_coverImg = CoverImage();
    g_coverCells.clear();
    g_coverGenW = g_coverGenH = 0;
    g_coverTrackIdx = idx;
    if (idx >= files.size()) return;
    g_coverImg = coverDecode(files[idx]);
}

// 读取音频标签(FFmpeg metadata): 返回 {title, artist, album}
struct TrackTags { std::string title, artist, album; };
TrackTags ReadTrackTags(const std::string& path) {
    TrackTags t;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) == 0) {
        AVDictionaryEntry* e = nullptr;
        if ((e = av_dict_get(fmt->metadata, "title", nullptr, AV_DICT_IGNORE_SUFFIX))) t.title = e->value;
        if ((e = av_dict_get(fmt->metadata, "artist", nullptr, AV_DICT_IGNORE_SUFFIX))) t.artist = e->value;
        if ((e = av_dict_get(fmt->metadata, "album", nullptr, AV_DICT_IGNORE_SUFFIX))) t.album = e->value;
        avformat_close_input(&fmt);
    }
    return t;
}

// 渲染时调用: 按终端尺寸动态生成色块(尺寸变化才重算)
void CoverEnsureCells(int termW, int termH) {
    // 封面: 高度占满右栏(底部 9 行预算: 歌名1+进度1+音量1+
    // 分隔1+controls 3+分隔1+状态1), 频谱在左栏不占右栏空间
    // 宽度 = min(右栏宽, 2×高); 字符格 ~1:1.8 下格子 1.8~2:1 显示均近正方形
    int wantRows = termH - 9;                     // 高度占满
    int wantCols = std::min(termW / 2 - 2, 2 * wantRows);  // 宽不超右栏
    if (wantCols < 16) wantCols = 16;
    if (wantRows < 8) wantRows = 8;
    // 宽度受限时(窄终端)高度跟随宽度保持 2:1
    if (wantCols < 2 * wantRows) wantRows = wantCols / 2;
    if (wantRows < 8) wantRows = 8;
    if ((wantCols != g_coverGenW || wantRows != g_coverGenH) && g_coverImg.valid()) {
        g_coverGenW = wantCols;
        g_coverGenH = wantRows;
        g_coverCells = coverCellsFromRgb(g_coverImg, wantCols, wantRows);
    }
}

// ---- 工具函数 ----

std::string FormatTime(double sec) {
    if (sec < 0) sec = 0;
    int total = static_cast<int>(sec);
    return std::to_string(total / 60) + ":" +
           (total % 60 < 10 ? "0" : "") + std::to_string(total % 60);
}

std::string FormatBitrate(int64_t br) {
    if (br >= 1000000) return std::to_string(br / 1000000) + "." +
                               std::to_string((br % 1000000) / 100000) + "Mbps";
    return std::to_string(br / 1000) + "kbps";
}

std::string BaseName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// 按显示宽度截断(中文/宽字符=2, 英文=1), 超长末尾加省略号 … (UTF-8 安全)
std::string TruncateW(const std::string& s, int maxW) {
    if (maxW <= 1) return "";
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        int cw = len >= 3 ? 2 : 1;   // 3+ 字节 = CJK/宽字符
        if (w + cw > maxW) break;
        w += cw;
        i += static_cast<size_t>(len);
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\u2026";  // …
}

// ---- LRC 歌词 ----

struct LrcLine {
    double time = 0.0;
    std::string text;
};

// 解析 LRC 时间标签: 支持 [mm:ss.xx] / [mm:ss:xx](帧, xx=1/100s) / [mm:ss]
// 成功返回秒数, 失败返回 -1
double ParseLrcTime(const std::string& tag) {
    int mm = 0, ss = 0;
    double frac = 0.0;
    char sep1 = 0;
    if (sscanf(tag.c_str(), "%d:%d%c%lf", &mm, &ss, &sep1, &frac) < 2) return -1.0;
    if (mm < 0 || ss < 0 || ss > 99) return -1.0;
    if (sep1 == '.') {
        // .xx 百分秒(2 位)或 .xxx 毫秒(3 位): 用数值判断
        if (frac >= 100.0) return mm * 60.0 + ss + frac / 1000.0;  // 毫秒
        return mm * 60.0 + ss + frac / 100.0;                      // 百分秒
    }
    if (sep1 == ':') {
        // [mm:ss:xx] 帧格式: xx 为 1/100 秒 (注意用 %lf 读完整数字, %c 只读 1 字符)
        double f2 = 0.0;
        if (sscanf(tag.c_str(), "%d:%d:%lf", &mm, &ss, &f2) == 3) {
            return mm * 60.0 + ss + f2 / 100.0;
        }
        return -1.0;
    }
    return mm * 60.0 + ss;
}

// 去首尾空白
std::string TrimStr(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// 解析 LRC 文本(健壮版):
// - 支持 [mm:ss.xx] / [mm:ss:xx] / [mm:ss] 时间标签, 一行多标签
// - 跳过元数据标签 [ti:]/[ar:]/[al:]/[by:]/[offset:]
// - 乱格式容错: 无时间标签的纯文本行收集为静态歌词(整段显示)
// - 时间标签后文本去首尾空白
std::vector<LrcLine> ParseLrcText(const std::string& content) {
    std::vector<LrcLine> out;
    int offsetMs = 0;  // [offset:+500] 全局偏移(毫秒, + 为歌词提前)
    std::string plainText;  // 无时间标签的纯文本(多行合并)
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() > 1 && line.back() == '\r') line.pop_back();
        std::vector<double> times;
        size_t pos = 0;
        size_t lastRb = std::string::npos;
        bool sawBracket = false;
        while (true) {
            size_t lb = line.find('[', pos);
            if (lb == std::string::npos) break;
            size_t rb = line.find(']', lb);
            if (rb == std::string::npos) break;
            std::string tag = line.substr(lb + 1, rb - lb - 1);
            pos = rb + 1;
            lastRb = rb;
            sawBracket = true;
            if (tag.empty()) continue;
            // offset 标签: [offset:+500] / [offset:-300]
            if (tag.rfind("offset", 0) == 0) {
                int ms = 0;
                if (sscanf(tag.c_str(), "offset:%d", &ms) == 1) offsetMs = ms;
                continue;
            }
            // 元数据标签 [ti:xx] [ar:xx] 等: 首字符非数字则跳过
            if (!(tag[0] >= '0' && tag[0] <= '9')) continue;
            double t = ParseLrcTime(tag);
            if (t >= 0.0) times.push_back(t);
        }
        if (!times.empty()) {
            // 时间标签后的文本(最后一个 ] 之后), 去首尾空白
            std::string text = (lastRb != std::string::npos) ? line.substr(lastRb + 1) : line;
            text = TrimStr(text);
            for (double t : times) {
                out.push_back({t, text});
            }
        } else if (!sawBracket) {
            // 整行无方括号: 可能是纯文本歌词
            std::string t = TrimStr(line);
            if (!t.empty()) {
                if (!plainText.empty()) plainText += "\n";
                plainText += t;
            }
        }
        // 有方括号但无有效时间(纯元数据行): 丢弃
    }
    // 应用全局偏移(毫秒 -> 秒; + 为提前即时间减)
    if (offsetMs != 0) {
        double off = static_cast<double>(offsetMs) / 1000.0;
        for (auto& l : out) l.time -= off;
    }
    std::sort(out.begin(), out.end(),
              [](const LrcLine& a, const LrcLine& b) { return a.time < b.time; });
    // 无时间标签的纯文本: 静态歌词(time=-1 标记, 渲染时整段显示)
    if (out.empty() && !plainText.empty()) {
        out.push_back({-1.0, plainText});
    }
    return out;
}

// 解析 .lrc 文件

std::vector<LrcLine> ParseLrcFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return ParseLrcText(content);
}

int CurrentLrcIndex(const std::vector<LrcLine>& lrc, double pos) {
    int idx = -1;
    for (size_t i = 0; i < lrc.size(); ++i) {
        if (lrc[i].time <= pos) idx = static_cast<int>(i);
        else break;
    }
    return idx;
}

// ---- 配置持久化(简易 JSON, 与 C++ 版 termus.json 同格式) ----

std::string ConfigPath() {
    const char* home = std::getenv("USERPROFILE");
    std::string dir = home ? std::string(home) : ".";
    // 配置统一放 ~/termus/ 目录(自动创建)
    std::string cfgDir = dir + "/termus";
    std::error_code ec;
    std::filesystem::create_directories(cfgDir, ec);
    return cfgDir + "/termus.json";
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void SaveConfig() {
    std::ofstream f(ConfigPath());
    if (!f) return;
    f << "{\n";
    f << "  \"volume\": " << g_player.volume() << ",\n";
    f << "  \"mode\": " << static_cast<int>(g_player.mode()) << ",\n";
    f << "  \"speed\": " << g_player.speed() << ",\n";
    f << "  \"eqEnabled\": " << (g_player.eqEnabled() ? "true" : "false") << ",\n";
    f << "  \"autoPlay\": " << (g_autoPlay ? "true" : "false") << ",\n";
    f << "  \"showLrcPanel\": " << (g_showLrcPanel ? "true" : "false") << ",\n";
    f << "  \"lastTrack\": \"" << JsonEscape(g_lastTrack) << "\",\n";
    f << "  \"lastPos\": " << g_lastPos << ",\n";
    f << "  \"eq\": [";
    for (int b = 0; b < kEqBands; ++b) {
        if (b) f << ", ";
        f << g_player.eqBand(b);
    }
    f << "],\n";
    f << "  \"favorites\": [";
    for (size_t i = 0; i < g_favorites.size(); ++i) {
        if (i) f << ", ";
        f << "\"" << JsonEscape(g_favorites[i]) << "\"";
    }
    f << "],\n";
    f << "  \"history\": [";
    for (size_t i = 0; i < g_history.size(); ++i) {
        if (i) f << ", ";
        f << "\"" << JsonEscape(g_history[i]) << "\"";
    }
    f << "]\n";
    f << "}\n";
}

void LoadConfig() {
    // 迁移: 新路径 ~/termus/termus.json 不存在时读旧路径 ~/termus.json
    std::string cfgPath = ConfigPath();
    const char* home = std::getenv("USERPROFILE");
    std::string legacy = (home ? std::string(home) : ".") + "/termus.json";
    if (!std::filesystem::exists(cfgPath) && std::filesystem::exists(legacy)) {
        cfgPath = legacy;
    }
    std::ifstream f(cfgPath);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    auto findNum = [&](const std::string& key, double& out) {
        auto p = content.find("\"" + key + "\"");
        if (p == std::string::npos) return false;
        p = content.find(':', p);
        if (p == std::string::npos) return false;
        out = std::atof(content.c_str() + p + 1);
        return true;
    };
    auto findBool = [&](const std::string& key, bool& out) {
        auto p = content.find("\"" + key + "\"");
        if (p == std::string::npos) return false;
        p = content.find(':', p);
        if (p == std::string::npos) return false;
        ++p;  // 跳过 ':'
        while (p < content.size() && (content[p] == ' ' || content[p] == '\t')) ++p;  // 跳过空格
        out = content.compare(p, 4, "true") == 0;
        return true;
    };
    auto findStr = [&](const std::string& key, std::string& out) {
        auto p = content.find("\"" + key + "\"");
        if (p == std::string::npos) return false;
        p = content.find(':', p);
        if (p == std::string::npos) return false;
        size_t q = content.find('"', p);
        if (q == std::string::npos) return false;
        size_t e = content.find('"', q + 1);
        if (e == std::string::npos) return false;
        out = content.substr(q + 1, e - q - 1);
        return true;
    };
    auto findArray = [&](const std::string& key, std::vector<std::string>& out) {
        auto p = content.find("\"" + key + "\"");
        if (p == std::string::npos) return false;
        p = content.find('[', p);
        if (p == std::string::npos) return false;
        size_t q = p + 1;
        while (true) {
            q = content.find('"', q);
            if (q == std::string::npos || q > content.find(']', p)) break;
            size_t e = content.find('"', q + 1);
            if (e == std::string::npos) break;
            out.push_back(content.substr(q + 1, e - q - 1));
            q = e + 1;
        }
        return true;
    };
    double d = 0;
    if (findNum("volume", d)) g_player.setVolume(static_cast<int>(d));
    if (findNum("mode", d)) {
        while (static_cast<int>(d) > 16) d -= 17;
        for (int i = 0; i < static_cast<int>(d); ++i) g_player.cycleMode();
    }
    if (findNum("speed", d)) g_player.setSpeed(static_cast<float>(d));
    bool b = false;
    g_player.setEqEnabled(true);  // 均衡器默认打开(配置有字段才覆盖)
    if (findBool("eqEnabled", b)) g_player.setEqEnabled(b);
    auto eqp = content.find("\"eq\"");
    if (eqp != std::string::npos) {
        auto lb = content.find('[', eqp);
        auto rb = content.find(']', eqp);
        if (lb != std::string::npos && rb != std::string::npos) {
            std::string arr = content.substr(lb + 1, rb - lb - 1);
            size_t s = 0;
            for (int i = 0; i < kEqBands; ++i) {
                float v = static_cast<float>(std::atof(arr.c_str() + s));
                g_player.setEqBand(i, v);
                s = arr.find(',', s);
                if (s == std::string::npos) break;
                ++s;
            }
        }
    }
    findArray("favorites", g_favorites);
    findArray("history", g_history);
    if (findBool("autoPlay", b)) g_autoPlay = b;
    if (findBool("showLrcPanel", b)) g_showLrcPanel = b;
    g_autoplaySel = g_autoPlay ? 1 : 0;
    g_showlrcSel = g_showLrcPanel ? 0 : 1;
    // 上次播放位置
    findStr("lastTrack", g_lastTrack);
    findNum("lastPos", g_lastPos);
}

// ---- 频谱可视化(cava 风格) ----

// cava 连续渐变: 绿(0,255,0) → 黄(255,255,0) → 橙(255,128,0) → 红(255,0,0)
// t: 0(底部)..1(顶部), 关键色线性插值(无分段色带)
Color cavaGradient(float t) {
    struct Stop { int r, g, b; };
    static const Stop stops[4] = {{0, 255, 0}, {255, 255, 0}, {255, 128, 0}, {255, 0, 0}};
    float x = t * 3.0f;
    int i = x < 3.0f ? static_cast<int>(x) : 2;
    float f = x - static_cast<float>(i);
    int r = static_cast<int>(stops[i].r + f * (stops[i + 1].r - stops[i].r));
    int g = static_cast<int>(stops[i].g + f * (stops[i + 1].g - stops[i].g));
    int b = static_cast<int>(stops[i].b + f * (stops[i + 1].b - stops[i].b));
    return Color::RGB(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

// cava 风格频谱: 全宽柱状图, 柱从底部向上生长, 高度渐变(绿→黄→橙→红),
// 频段对数分布(低音左/高音右), 数据侧已做攻击快/释放慢平滑(近似 cava 的 peak 下落)。
// height = 柱高(行数), totalWidth = 可用列宽(每柱 2 列: 柱 + 间隙)。
Element cavaSpectrumViz(int height, int totalWidth) {
    std::vector<Element> bars;
    float levels[Player::kSpectrumBands];
    g_player.spectrumLevels(levels, Player::kSpectrumBands);
    const int n = Player::kSpectrumBands;
    const int cols = std::max(8, totalWidth);  // 每柱 1 列, 左右无间隙
    for (int i = 0; i < cols; ++i) {
        // 12 频段线性插值到 cols 柱; 跳过频段 0-1(60-148Hz 人声歌恒 0, 左边缘不空)
        float t = 2.0f + static_cast<float>(i) / cols * (n - 2);
        int b0 = static_cast<int>(t);
        int b1 = std::min(b0 + 1, n - 1);
        float frac = t - static_cast<float>(b0);
        float lv = levels[b0] * (1.0f - frac) + levels[b1] * frac;
        // 低音区(左 1/5 柱)增益 ×1.5(不饱和, 留跳动空间)
        if (i < cols / 5) lv = std::min(1.0f, lv * 1.5f);
        // cava 风格: sqrt 非线性放大低电平(无额外增益, 柱随数据变化不钉死)
        float lv2 = std::sqrt(std::min(1.0f, lv));
        // 根据强度细分: 柱高直接按半格取整(0..2*height), 奇数半格=半块, 偶数=整格
        int units = static_cast<int>(lv2 * height * 2.0f + 0.5f);
        if (units < 0) units = 0;  // 无数据不显示(不要默认绿条)
        if (units > height * 2) units = height * 2;
        int fillFrom = height * 2 - units;  // 填充起始半格(从底部数)
        const float maxHalf = 2.0f * height - 1.0f;
        std::vector<Element> col;
        for (int r = 0; r < height; ++r) {
            int hTop = 2 * r;        // 上半半格(0=顶)
            int hBot = 2 * r + 1;    // 下半半格
            bool fillTop = hTop >= fillFrom;
            bool fillBot = hBot >= fillFrom;
            float posTop = maxHalf > 0 ? 1.0f - hTop / maxHalf : 0.0f;
            float posBot = maxHalf > 0 ? 1.0f - hBot / maxHalf : 0.0f;
            Element e;
            if (fillTop && fillBot) {
                // 整格: 上半=浅色(顶红), 下半=深色(底绿), 2 色碎块
                e = text("▀") | color(cavaGradient(posTop)) | bgcolor(cavaGradient(posBot));
            } else if (fillBot) {
                // 半块: 只填下半(柱顶半格), 上半留背景
                e = text("▄") | color(cavaGradient(posBot));
            } else {
                e = text(" ");
            }
            col.push_back(e);
        }
        bars.push_back(vbox(std::move(col)) | flex);
    }
    return hbox(bars);
}

// ---- EQ 均衡器条(设置页与主界面共用; Kaula 版补齐, 与 C++ 版一致) ----
Element eqBars(const Player& player) {
    if (!player.eqEnabled()) return text("");
    std::vector<Element> bars;
    for (int b = 0; b < kEqBands; ++b) {
        float g = player.eqBand(b);
        float frac = (g + 12.0f) / 24.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        bars.push_back(vbox({
            text(std::to_string(static_cast<int>(g))) | dim | center,
            gauge(frac) | color(g >= 0.0f ? Color::Green : Color::Yellow) |
                size(HEIGHT, EQUAL, 1) | flex,
        }) | flex);
    }
    return hbox(bars);
}

// 歌词渲染(高亮当前行)
Element lrcRender(const std::vector<LrcLine>& lrc, double pos, int align = 1) {
    if (lrc.empty()) {
        return text(" (无歌词, 放置同名 .lrc 文件)") | dim;
    }
    // 静态歌词(无时间标签, time=-1): 整段显示, 不随播放滚动
    if (lrc[0].time < 0) {
        return paragraph(lrc[0].text) | dim | flex_grow;
    }
    int idx = CurrentLrcIndex(lrc, pos);
    int start = 0, end = 0;
    if (idx >= 0) {
        start = std::max(0, idx - 4);
        end = std::min(static_cast<int>(lrc.size()), idx + 5);
    } else if (!lrc.empty()) {
        end = std::min(static_cast<int>(lrc.size()), 5);
    }
    std::vector<Element> lines;
    for (int i = start; i < end; ++i) {
        Element line;
        if (i == idx) {
            line = text("▶ " + lrc[static_cast<size_t>(i)].text) | bold | color(Color::Cyan);
        } else {
            line = text("  " + lrc[static_cast<size_t>(i)].text) | dim;
        }
        // 对齐: 0=居左 1=居中 2=居右
        if (align == 1) {
            lines.push_back(hbox({text(""), line | xflex_grow, text("")}));
        } else if (align == 2) {
            lines.push_back(hbox({text(""), text("") | xflex_grow, line}));
        } else {
            lines.push_back(line);
        }
    }
    return vbox(lines);
}

// 静态字符串缓冲(线程不安全但只被 UI 线程读取)
std::string g_titleBuf, g_modeBuf, g_codecBuf, g_eqPresetBuf, g_favBuf;

// 注册的 Kaula 回调
termus_cb_play   g_cb_play = nullptr;
termus_cb_void   g_cb_toggle = nullptr;
termus_cb_void   g_cb_stop = nullptr;
termus_cb_void   g_cb_next = nullptr;
termus_cb_void   g_cb_prev = nullptr;
termus_cb_void   g_cb_mode = nullptr;
termus_cb_seek   g_cb_seek = nullptr;
termus_cb_volume g_cb_volume = nullptr;
termus_cb_void   g_cb_speed = nullptr;
termus_cb_void   g_cb_eq = nullptr;
termus_cb_void   g_cb_eq_preset = nullptr;
termus_cb_void   g_cb_refresh = nullptr;
termus_cb_void   g_cb_fav = nullptr;
termus_cb_void   g_cb_exit = nullptr;

// UI 状态
int g_selected = 0;
float g_uiVolume = 80.0f;

// EQ 预设(10 段, dB); 均衡器效果精简为 4 个: 默认/古典/摇滚/流行
const char* kEqPresetNames[] = {"默认", "古典", "摇滚", "流行"};
constexpr int kEqPresetCount = 4;
const float kEqPresets[kEqPresetCount][kEqBands] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},   // 默认
    {4, 3, 2, 1, 0, -1, -1, 0, 2, 3}, // 古典
    {5, 4, 2, -1, -2, -3, -2, -1, 1, 2}, // 摇滚
    {-1, 1, 3, 4, 2, 0, -1, -1, 2, 3}, // 流行
};
int g_eqPresetIdx = 0;

// 扫描音频目录(与 ter-music 一致的扩展名集合)
std::vector<std::string> ScanAudioDir(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" ||
            ext == ".m4a" || ext == ".aac" || ext == ".opus" || ext == ".ape" ||
            ext == ".wv" || ext == ".wma") {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool IsFavorite(const std::string& path) {
    return std::find(g_favorites.begin(), g_favorites.end(), path) != g_favorites.end();
}

}  // namespace

// ==================== 播放器控制 ====================

void termus_play(int64_t idx) {
    if (g_files.empty()) return;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int64_t>(g_files.size())) idx = 0;
    g_player.play(static_cast<size_t>(idx));
}

void termus_toggle(void) { g_player.toggle(); }
void termus_stop(void) { g_player.stop(); }
void termus_next(void) { g_player.next(); }
void termus_prev(void) { g_player.prev(); }
void termus_cycle_mode(void) { g_player.cycleMode(); }
void termus_seek(double sec) { g_player.seek(sec); }
void termus_set_volume(int64_t pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_player.setVolume(static_cast<int>(pct));
    g_uiVolume = static_cast<float>(pct);
}

void termus_cycle_speed(void) {
    static const float kSpeeds[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
    float cur = g_player.speed();
    for (size_t i = 0; i < 6; ++i) {
        if (cur < kSpeeds[i] - 0.01f) { g_player.setSpeed(kSpeeds[i]); return; }
    }
    g_player.setSpeed(kSpeeds[0]);
}

void termus_set_eq_enabled(int64_t on) { g_player.setEqEnabled(on != 0); }

void termus_cycle_eq_preset(void) {
    g_eqPresetIdx = (g_eqPresetIdx + 1) % kEqPresetCount;
    g_player.setEqEnabled(true);
    g_player.setEqPreset(kEqPresets[g_eqPresetIdx]);
}

// ==================== 状态查询 ====================

int64_t termus_state(void) { return static_cast<int64_t>(g_player.state()); }
double termus_position(void) { return g_player.position(); }
double termus_duration(void) { return g_player.duration(); }
int64_t termus_current_index(void) { return static_cast<int64_t>(g_player.currentIndex()); }

const char* termus_title(void) {
    g_titleBuf = g_hasPlaylist ? BaseName(g_player.currentTitle()) : "(无播放列表)";
    return g_titleBuf.c_str();
}
const char* termus_mode_name(void) {
    g_modeBuf = g_player.modeName();
    return g_modeBuf.c_str();
}
int64_t termus_volume(void) { return g_player.volume(); }
double termus_speed(void) { return g_player.speed(); }
int64_t termus_eq_enabled(void) { return g_player.eqEnabled() ? 1 : 0; }
const char* termus_eq_preset_name(void) {
    g_eqPresetBuf = kEqPresetNames[g_eqPresetIdx];
    return g_eqPresetBuf.c_str();
}
const char* termus_codec_info(void) {
    g_codecBuf = g_player.codecName() + " | " +
                 std::to_string(g_player.sampleRate()) + "Hz | " +
                 std::to_string(g_player.bitDepth()) + "bit | " +
                 FormatBitrate(g_player.bitRate());
    return g_codecBuf.c_str();
}

// ==================== 播放列表 ====================

int64_t termus_playlist_size(void) { return static_cast<int64_t>(g_files.size()); }

const char* termus_playlist_name(int64_t i) {
    if (i < 0 || i >= static_cast<int64_t>(g_files.size())) return "";
    g_titleBuf = BaseName(g_files[static_cast<size_t>(i)]);
    return g_titleBuf.c_str();
}

const char* termus_playlist_path(int64_t i) {
    if (i < 0 || i >= static_cast<int64_t>(g_files.size())) return "";
    return g_files[static_cast<size_t>(i)].c_str();
}

int64_t termus_playlist_refresh(const char* dir) {
    g_scanDir = dir ? dir : "";
    g_files = g_scanDir.empty() ? std::vector<std::string>() : ScanAudioDir(g_scanDir);
    g_player.setPlaylist(g_files);
    g_hasPlaylist = !g_files.empty();
    if (g_selected >= static_cast<int>(g_files.size())) g_selected = 0;
    // 重建显示条目: Menu 引用 g_entries, 刷新后 UI 立即更新
    g_entries.clear();
    for (size_t i = 0; i < g_files.size(); ++i) {
        g_entries.push_back("  " + std::to_string(i + 1) + ". " + BaseName(g_files[i]));
    }
    return static_cast<int64_t>(g_files.size());
}

// ==================== 收藏 ====================

void termus_fav_toggle(void) {
    if (!g_hasPlaylist) return;
    size_t idx = g_player.currentIndex();
    if (idx >= g_files.size()) return;
    const std::string& path = g_files[idx];
    auto it = std::find(g_favorites.begin(), g_favorites.end(), path);
    if (it != g_favorites.end()) {
        g_favorites.erase(it);
    } else {
        g_favorites.push_back(path);
    }
}

int64_t termus_fav_count(void) { return static_cast<int64_t>(g_favorites.size()); }

const char* termus_fav_name(int64_t i) {
    if (i < 0 || i >= static_cast<int64_t>(g_favorites.size())) return "";
    g_favBuf = BaseName(g_favorites[static_cast<size_t>(i)]);
    return g_favBuf.c_str();
}

// ==================== UI 生命周期 ====================

ScreenInteractive* g_screen = nullptr;

void termus_ui_exit(void) {
    if (g_screen) g_screen->Exit();
}

void termus_ui_start(termus_cb_play cb_play, termus_cb_void cb_toggle,
                     termus_cb_void cb_stop, termus_cb_void cb_next,
                     termus_cb_void cb_prev, termus_cb_void cb_mode,
                     termus_cb_seek cb_seek, termus_cb_volume cb_volume,
                     termus_cb_void cb_speed, termus_cb_void cb_eq,
                     termus_cb_void cb_eq_preset, termus_cb_void cb_refresh,
                     termus_cb_void cb_fav, termus_cb_void cb_exit) {
    // 兜底: 强制 FTXUI 按 24-bit 真彩色处理(WT/conhost 不设 TERM/COLORTERM,
    // FTXUI 会降级到 16 色量化导致封面发灰; 编译宏 FTXUI_MICROSOFT_TERMINAL_FALLBACK 已覆盖,
    // 这里再显式设 COLORTERM 双保险)
    _putenv("COLORTERM=truecolor");
    SetConsoleTitleW(L"termus-kaula");
    // 强制控制台字号 10 (仅 conhost: WT 里有 WT_SESSION 变量则跳过, 由 profile 控制)
    if (!getenv("WT_SESSION")) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_FONT_INFOEX cfi = {};
        cfi.cbSize = sizeof(CONSOLE_FONT_INFOEX);
        cfi.dwFontSize = {10, 18};      // 10 号 (conhost 标准字号 10x18)
        cfi.FontFamily = 0x36;          // FF_DONTCARE | TMPF_TRUETYPE
        cfi.FontWeight = 400;
        wcscpy(cfi.FaceName, L"Consolas");
        SetCurrentConsoleFontEx(hOut, FALSE, &cfi);
    }
    // 自动全屏: 由启动器负责 (wt --maximized 或 .lnk 最大化运行)
    // 程序内 ShowWindow 会改变终端尺寸, 干扰 FTXUI Fullscreen 事件循环(点击无响应)
    g_cb_play = cb_play;
    g_cb_toggle = cb_toggle;
    g_cb_stop = cb_stop;
    g_cb_next = cb_next;
    g_cb_prev = cb_prev;
    g_cb_mode = cb_mode;
    g_cb_seek = cb_seek;
    g_cb_volume = cb_volume;
    g_cb_speed = cb_speed;
    g_cb_eq = cb_eq;
    g_cb_eq_preset = cb_eq_preset;
    g_cb_refresh = cb_refresh;
    g_cb_fav = cb_fav;
    g_cb_exit = cb_exit;

    // 初始化音频输出 (waveOut)。不调用则 hwo_ 为空, AudioOut::start 静默失败 → 无声
    g_player.initOutput();

    // 加载持久化配置(音量/模式/速度/EQ/收藏/历史/设置项)
    LoadConfig();
    if (g_autoPlay && !g_files.empty()) {
        // 优先恢复上次曲目+位置(原版风格), 否则第一首
        auto it = std::find(g_files.begin(), g_files.end(), g_lastTrack);
        if (it != g_files.end()) {
            size_t idx = static_cast<size_t>(it - g_files.begin());
            g_player.play(idx);
            if (g_lastPos > 1.0) g_player.seek(g_lastPos);  // 避免从 0 开始
            g_history.insert(g_history.begin(), g_files[idx]);
            if (g_history.size() > 100) g_history.pop_back();
        } else {
            g_player.play(0);
            g_history.insert(g_history.begin(), g_files[0]);
            if (g_history.size() > 100) g_history.pop_back();
        }
        // 立即加载当前曲目封面(不等 ticker; 歌词在 loadLrcFor 定义后加载)
        CoverLoadFor(g_player.currentIndex(), g_files);
    }

    auto screen = ScreenInteractive::Fullscreen();
    g_screen = &screen;

    // 歌词状态
    std::vector<LrcLine> lrc;
    std::string lrcFile;
    size_t lrcTrackIndex = SIZE_MAX;
    bool lrcPending = false;   // 歌词加载挂起(内嵌解码异步, 失败后重试)
    int lrcRetry = 0;          // 重试计数(上限 8 次 ≈2s)
    int lrcSource = 0;  // 0=自动 1=外置 2=内嵌
    // 播放历史弹窗(H 键, 原版样式): 最近 10 首, 可上下选择回车播放
    bool showHistory = false;
    int histSel = 0;

    // 歌词对齐: 0=左 1=中 2=右(设置页)
    int lrcAlignSel = 1;

    // ---- 播放队列(原版 play_queue): a=追加 i=插入 d=移除 D=清空 J/K=移动 ----
    std::vector<std::string> queue;
    int queueSel = 0;
    bool showQueue = false;
    auto queueAdd = [&](size_t idx) {
        if (idx < g_files.size()) queue.push_back(g_files[idx]);
    };
    auto queueInsert = [&](size_t idx) {
        if (idx < g_files.size())
            queue.insert(queue.begin() + std::min<size_t>(queueSel, queue.size()), g_files[idx]);
    };
    auto queueRemove = [&] {
        if (!queue.empty() && queueSel >= 0 && static_cast<size_t>(queueSel) < queue.size()) {
            queue.erase(queue.begin() + queueSel);
            if (queueSel >= static_cast<int>(queue.size())) queueSel = static_cast<int>(queue.size()) - 1;
            if (queueSel < 0) queueSel = 0;
        }
    };

    // ---- 音乐库浏览器(M 键): 艺术家→专辑→曲目 ----
    std::map<std::string, TrackTags> tagCache;
    std::vector<std::string> libArtists;
    std::vector<std::string> libAlbums;
    std::vector<size_t> libTracks;
    int libSel = 0;
    bool showLib = false;
    int libLevel = 0;
    int libArtistIdx = 0;
    auto buildLibArtists = [&] {
        tagCache.clear();
        libArtists.clear();
        std::map<std::string, bool> seen;
        std::vector<std::string> albums(g_files.size()), artists(g_files.size());
        for (size_t i = 0; i < g_files.size(); ++i) {
            TrackTags t = ReadTrackTags(g_files[i]);
            tagCache[g_files[i]] = t;
            albums[i] = t.album;
            artists[i] = t.artist;
            if (t.artist.empty()) t.artist = "未知艺术家";
            if (!seen[t.artist]) { seen[t.artist] = true; libArtists.push_back(t.artist); }
        }
        g_player.setTagCache(albums, artists);
        std::sort(libArtists.begin(), libArtists.end());
        libSel = 0;
    };
    auto openLibArtist = [&](int idx) {
        libAlbums.clear();
        std::map<std::string, bool> seen;
        for (auto& f : g_files) {
            auto it = tagCache.find(f);
            if (it == tagCache.end()) continue;
            std::string artist = it->second.artist.empty() ? "未知艺术家" : it->second.artist;
            if (artist != libArtists[static_cast<size_t>(idx)]) continue;
            std::string album = it->second.album.empty() ? "未知专辑" : it->second.album;
            if (!seen[album]) { seen[album] = true; libAlbums.push_back(album); }
        }
        std::sort(libAlbums.begin(), libAlbums.end());
        libSel = 0;
    };
    auto openLibAlbum = [&](int artistIdx, int albumIdx) {
        libTracks.clear();
        for (size_t i = 0; i < g_files.size(); ++i) {
            auto it = tagCache.find(g_files[i]);
            if (it == tagCache.end()) continue;
            std::string artist = it->second.artist.empty() ? "未知艺术家" : it->second.artist;
            std::string album = it->second.album.empty() ? "未知专辑" : it->second.album;
            if (artist == libArtists[static_cast<size_t>(artistIdx)] &&
                album == libAlbums[static_cast<size_t>(albumIdx)]) {
                libTracks.push_back(i);
            }
        }
        libSel = 0;
    };

    // ---- 自定义歌单(F4 管理 + A 加入) ----
    struct Playlist { std::string name; std::vector<std::string> tracks; };
    std::vector<Playlist> playlists;
    int plSel = 0;
    bool showPlaylists = false;
    auto playlistsPath = [] {
        const char* home = std::getenv("USERPROFILE");
        std::string dir = home ? std::string(home) : ".";
        // 歌单与配置同目录 ~/termus/
        std::string cfgDir = dir + "/termus";
        std::error_code ec;
        std::filesystem::create_directories(cfgDir, ec);
        return cfgDir + "/playlists.json";
    };
    auto savePlaylists = [&] {
        std::ofstream f(playlistsPath());
        if (!f) return;
        f << "[\n";
        for (size_t i = 0; i < playlists.size(); ++i) {
            f << "  {\"name\": \"" << JsonEscape(playlists[i].name) << "\", \"tracks\": [";
            for (size_t j = 0; j < playlists[i].tracks.size(); ++j) {
                if (j) f << ", ";
                f << "\"" << JsonEscape(playlists[i].tracks[j]) << "\"";
            }
            f << "]}";
            if (i + 1 < playlists.size()) f << ",";
            f << "\n";
        }
        f << "]\n";
    };
    auto loadPlaylists = [&] {
        // 迁移: 新路径 ~/termus/playlists.json 不存在时读旧路径 ~/termus_playlists.json
        std::string plPath = playlistsPath();
        const char* home = std::getenv("USERPROFILE");
        std::string legacy = (home ? std::string(home) : ".") + "/termus_playlists.json";
        if (!std::filesystem::exists(plPath) && std::filesystem::exists(legacy)) {
            plPath = legacy;
        }
        std::ifstream f(plPath);
        if (!f) return;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        playlists.clear();
        size_t p = 0;
        while ((p = content.find("{\"name\": \"", p)) != std::string::npos) {
            size_t q = content.find('"', p + 10);
            if (q == std::string::npos) break;
            Playlist pl;
            pl.name = content.substr(p + 10, q - p - 10);
            size_t lb = content.find('[', q);
            if (lb == std::string::npos) break;
            size_t rb = content.find(']', lb);
            if (rb == std::string::npos) break;
            size_t t = lb;
            while ((t = content.find('"', t + 1)) < rb) {
                size_t e = content.find('"', t + 1);
                if (e == std::string::npos || e > rb) break;
                pl.tracks.push_back(content.substr(t + 1, e - t - 1));
                t = e;
            }
            playlists.push_back(std::move(pl));
            p = rb;
        }
    };
    auto addToPlaylist = [&](int plIdx) {
        if (plIdx < 0 || plIdx >= static_cast<int>(playlists.size())) return;
        if (g_selected < 0 || static_cast<size_t>(g_selected) >= g_files.size()) return;
        auto& pl = playlists[static_cast<size_t>(plIdx)];
        std::string path = g_files[static_cast<size_t>(g_selected)];
        if (std::find(pl.tracks.begin(), pl.tracks.end(), path) == pl.tracks.end()) {
            pl.tracks.push_back(path);
            savePlaylists();
        }
    };
    // 首页加入歌单覆盖层(A/a 键)
    bool showPlAdd = false;
    int plAddSel = 0;
    auto plAddTo = [&](int plIdx) {
        addToPlaylist(plIdx);
        showPlAdd = false;
    };
    // 歌单重命名(歌单管理页按 r): 手动输入, 不依赖 Input 组件焦点
    std::string plRenameBuf;
    bool showPlRename = false;
    // 折叠式歌单管理: plExpanded = 展开的歌单索引(-1 无)
    int plExpanded = -1;
    auto plRowOf = [&](int row, int& plIdx, int& trIdx) {
        int r = 0;
        for (size_t i = 0; i < playlists.size(); ++i) {
            if (r == row) { plIdx = static_cast<int>(i); trIdx = -1; return true; }
            r++;
            if (plExpanded == static_cast<int>(i)) {
                for (size_t j = 0; j < playlists[i].tracks.size(); ++j) {
                    if (r == row) { plIdx = static_cast<int>(i); trIdx = static_cast<int>(j); return true; }
                    r++;
                }
            }
        }
        return false;
    };
    auto plRowCount = [&] {
        int n = 0;
        for (size_t i = 0; i < playlists.size(); ++i) {
            n++;
            if (plExpanded == static_cast<int>(i)) n += static_cast<int>(playlists[i].tracks.size());
        }
        return n;
    };
    auto plRemoveTrack = [&](int plIdx, int trIdx) {
        if (plIdx < 0 || plIdx >= static_cast<int>(playlists.size())) return;
        auto& pl = playlists[static_cast<size_t>(plIdx)];
        if (trIdx >= 0 && trIdx < static_cast<int>(pl.tracks.size())) {
            pl.tracks.erase(pl.tracks.begin() + trIdx);
            savePlaylists();
        }
    };
    loadPlaylists();

    // ---- 搜索(/ 键, 原版样式): 过滤播放列表 ----
    std::string searchQuery;
    std::vector<std::string> searchResults;
    auto doSearch = [&] {
        searchResults.clear();
        if (searchQuery.empty()) {
            for (auto& f : g_files) searchResults.push_back(f);
            return;
        }
        std::string q = searchQuery;
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // 查询是纯 ASCII 时也匹配拼音首字母(如 "tn" 找 童年)
        std::string qAbbr = (searchQuery.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
            ? PinyinAbbr(searchQuery) : std::string();
        for (auto& f : g_files) {
            std::string name = BaseName(f);
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find(q) != std::string::npos) { searchResults.push_back(f); continue; }
            // 拼音首字母匹配: "童年" 的 abbr="tn", 查询 "tn" 命中
            if (!qAbbr.empty()) {
                std::string abbr = PinyinAbbr(name);
                if (abbr.find(qAbbr) != std::string::npos) searchResults.push_back(f);
            }
        }
    };

    // ---- 帮助弹窗(? 键): 快捷键说明 ----
    bool showHelp = false;
    const std::vector<std::string> helpLines = {
        " ♫ Termus 快速上手 ",
        " ──────────────── ",
        " 播放:   回车=播放选中  空格=播放/暂停",
        " 导航:   ↑/↓=选择曲目  ←/→=快退/快进5秒",
        " 音量:   +/=增大  -=减小",
        " 模式:   M=切换播放模式",
        " 速度:   ⏩ 按钮循环 0.75x~3.0x",
        " EQ:     🎛 开关  🎚 预设循环",
        " 列表:   R=刷新  🔄 按钮",
        " 收藏:   ♥ 按钮  (配置自动保存)",
        " 历史:   H=播放历史弹窗",
        " 睡眠:   T=定时器(30/60/90分钟循环, 到点暂停)",
        " 详情:   i=当前歌曲信息弹窗",
        " 搜索:   /=搜索过滤播放列表",
        " 歌词:   Ctrl+L=切换歌词来源(自动/外置/内嵌)",
        " 设置:   S=进入/退出设置页",
        " 退出:   Q 或 ✕ 按钮",
        " 双击:   列表双击曲目播放",
        " ──────────────── ",
        " Esc=关闭本帮助",
    };
    auto lrcSourceName = [](int s) -> const char* {
        return s == 0 ? "自动" : (s == 1 ? "外置" : "内嵌");
    };
    auto loadLrcFor = [&](size_t idx) {
        if (idx >= g_files.size()) { lrc.clear(); return; }
        std::string base = g_files[idx];
        std::string lrcPath = base.substr(0, base.find_last_of('.')) + ".lrc";
        lrcFile = lrcPath;
        std::string emb = g_player.embeddedLyrics();
        // 解码线程异步 openCurrent 填充内嵌歌词: 空则靠 pending 重试
        // (ticker 250ms 重试, 不在此 sleep——UI 线程 sleep 会冻结界面)
        bool haveEmb = !emb.empty();
        std::vector<LrcLine> ext = ParseLrcFile(lrcPath);
        // 内嵌+外置都拿不到: 挂起重试(最多 8 次 ≈2s, 防纯音乐无限重试卡 UI)
        if (!haveEmb && ext.empty()) {
            if (++lrcRetry < 8) lrcPending = true;
            else { lrcPending = false; lrcRetry = 0; }
        } else {
            lrcPending = false;
            lrcRetry = 0;
        }
        if (lrcSource == 1) {
            lrc = ext;
        } else {
            lrc = haveEmb ? ParseLrcText(emb) : ext;
        }
    };

    // 启动 autoplay 已播放: 立即加载当前曲目歌词(不等 ticker, 保证首屏显示;
    // 内嵌歌词异步 open, 拿不到时 lrcPending 重试兜底)
    if (g_player.state() != Player::State::Stopped && !g_files.empty()) {
        loadLrcFor(g_player.currentIndex());
    }

    // ---- 可点击/拖动条组件(进度条/音量条共用, 同 C++ 版) ----
    // 样式: 无方括号, 已播放=█, 未播放=░; 鼠标按下/拖动按比例触发 onAct(frac)
    // 交互模式同 FTXUI Slider: OnRender 用 reflect 记录框, CaptureMouse 捕获拖动
    class ClickBar : public ComponentBase {
    public:
        ClickBar(std::function<void(double)> onAct, std::function<double()> getFrac,
                 std::function<int()> getWidth, Color fillColor,
                 std::function<std::string()> getLeft,
                 std::function<std::string()> getTail)
            : onAct_(std::move(onAct)), getFrac_(std::move(getFrac)),
              getWidth_(std::move(getWidth)), fillColor_(fillColor),
              getLeft_(std::move(getLeft)), getTail_(std::move(getTail)) {}

        Element OnRender() override {
            int avail = getWidth_();
            if (avail < 8) avail = 8;
            double frac = getFrac_();
            int filled = static_cast<int>(frac * avail);
            if (filled < 0) filled = 0;
            if (filled > avail) filled = avail;
            std::string f, e;
            for (int i = 0; i < filled; ++i) f += "█";
            for (int i = 0; i < avail - filled; ++i) e += "░";
            return hbox({
                       text(" "),
                       text(getLeft_()) | dim | size(WIDTH, EQUAL, 10),
                       text(" "),
                       text(f) | color(fillColor_),
                       text(e) | color(Color::GrayDark),
                       text(" "),
                       text(getTail_()) | dim,
                   }) |
                   reflect(gauge_box_);
        }

        bool OnEvent(Event event) override {
            if (captured_mouse_) {
                if (event.is_mouse() && event.mouse().motion == Mouse::Released) {
                    captured_mouse_ = nullptr;
                    return true;
                }
                if (event.is_mouse()) return OnAct(event);
            }
            if (event.is_mouse() && event.mouse().button == Mouse::Left &&
                event.mouse().motion == Mouse::Pressed &&
                gauge_box_.Contain(event.mouse().x, event.mouse().y)) {
                captured_mouse_ = CaptureMouse(event);
                if (captured_mouse_) {
                    TakeFocus();
                    return OnAct(event);
                }
            }
            return ComponentBase::OnEvent(event);
        }

        bool Focusable() const override { return true; }

    private:
        bool OnAct(Event event) {
            int w = gauge_box_.x_max - gauge_box_.x_min;
            if (w > 0) {
                double frac = static_cast<double>(event.mouse().x - gauge_box_.x_min) / w;
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
                onAct_(frac);
                return true;
            }
            return false;
        }

        std::function<void(double)> onAct_;
        std::function<double()> getFrac_;
        std::function<int()> getWidth_;
        Color fillColor_;
        std::function<std::string()> getLeft_;
        std::function<std::string()> getTail_;
        Box gauge_box_;
        CapturedMouse captured_mouse_;
    };

    // 播放列表条目: Menu 直接引用全局 g_entries (refresh 时重建, UI 即更新)
    // 正在播放的行: 黄色粗体 + ▶ 前缀高亮(选中项仍为反色)
    MenuOption menu_opt;
    menu_opt.entries_option.transform = [&](const EntryState& state) {
        // 正在播放的行: ▶ 前缀 + 黄色粗体(即使选中也保持黄色, 不用反色)
        bool isPlaying = (static_cast<size_t>(state.index) == g_player.currentIndex());
        Element e = text(isPlaying ? "▶" + state.label : state.label);
        if (isPlaying) e = e | bold | color(Color::Yellow);
        else if (state.active) e = e | inverted;
        return e;
    };
    auto playlist = Menu(&g_entries, &g_selected, menu_opt);
    auto volume_slider = Slider(" 音量", &g_uiVolume, 0.0f, 100.0f, 1.0f);
    // 右栏宽度(渲染器每帧更新; 供进度条/音量条/封面计算)
    int termW = 120;
    // 进度条(可点击/拖动 seek; 左侧显示时间数字, 右侧无尾标)
    auto seek_bar = Make<ClickBar>(
        [&](double frac) { g_player.seek(frac * g_player.duration()); },
        [&]() -> double {
            return g_player.duration() > 0.0
                       ? g_player.position() / g_player.duration() : 0.0;
        },
        [&]() -> int { return termW / 2 - 12; },
        Color::Cyan,
        [&]() -> std::string {
            return FormatTime(g_player.position()) + "/" + FormatTime(g_player.duration());
        },
        [&]() -> std::string { return ""; });
    // 音量条(可点击/拖动; 仅左侧数字, 无百分比)
    auto volume_bar = Make<ClickBar>(
        [&](double frac) {
            g_uiVolume = static_cast<float>(frac * 100.0);
            if (g_cb_volume) g_cb_volume(static_cast<int>(g_uiVolume));
        },
        [&]() -> double { return g_uiVolume / 100.0; },
        [&]() -> int { return termW / 2 - 12; },
        Color::Green,
        [&]() -> std::string {
            return "音量 " + std::to_string(static_cast<int>(g_uiVolume));
        },
        [&]() -> std::string { return ""; });

    // 播放/暂停合一(双状态, 同 C++ 版): 点击切换, 文本随播放状态变化
    std::string playLabel = " ▶ 播放 ";
    auto updatePlayPauseLabel = [&] {
        playLabel = (g_player.state() == Player::State::Playing)
            ? " ⏸ 暂停 "
            : " ▶ 播放 ";
    };
    auto btn_play_pause = Button(&playLabel, [&] {
        if (g_cb_toggle) g_cb_toggle();
        updatePlayPauseLabel();  // 立即刷新按钮文本
    });
    auto btn_prev = Button(" ⏮ 上一首 ", [&] { if (g_cb_prev) g_cb_prev(); });
    auto btn_next = Button(" ⏭ 下一首 ", [&] { if (g_cb_next) g_cb_next(); });
    auto btn_stop = Button(" ⏹ 停止 ", [&] { if (g_cb_stop) g_cb_stop(); });
    auto btn_mode = Button(" 🔁 模式 ", [&] { if (g_cb_mode) g_cb_mode(); });
    auto btn_speed = Button(" ⏩ 速度 ", [&] { if (g_cb_speed) g_cb_speed(); });
    auto btn_eq_preset = Button(" 🎚 效果 ", [&] { if (g_cb_eq_preset) g_cb_eq_preset(); });
    auto btn_refresh = Button(" 🔄 刷新 ", [&] { if (g_cb_refresh) g_cb_refresh(); });
    auto btn_fav = Button(" ♥ 收藏 ", [&] { if (g_cb_fav) g_cb_fav(); });
    auto btn_exit = Button(" ✕ 退出 ", [&] { SaveConfig(); screen.Exit(); });

    auto controls = Container::Horizontal({
        btn_play_pause, btn_prev, btn_next, btn_stop, btn_mode,
        btn_speed, btn_eq_preset, btn_refresh, btn_fav, btn_exit,
    });
    auto right_panel = Container::Vertical({volume_slider, seek_bar, volume_bar});
    auto layout = Container::Vertical({
        Container::Horizontal({playlist, right_panel}),
        controls,
    });

    // ---- 设置页组件 ----
    std::vector<std::string> autoplay_entries = {"关", "开"};
    std::vector<std::string> lrcalign_entries = {"居左", "居中", "居右"};
    auto toggle_autoplay = Toggle(autoplay_entries, &g_autoplaySel);
    auto toggle_lrcalign = Toggle(lrcalign_entries, &lrcAlignSel);
    auto btn_clear_hist = Button(" 清空历史 ", [&] {
        g_history.clear();
    });
    auto btn_settings_back = Button(" ◀ 返回主界面 ", [&] { g_viewSel = 0; });
    auto settings_panel = Container::Vertical({
        toggle_autoplay,
        toggle_lrcalign,
        btn_clear_hist,
        btn_settings_back,
    });
    // ---- 搜索视图(/ 键): 输入框 + 结果列表 ----
    auto search_input = Input(&searchQuery, "输入关键词过滤...");
    int searchSel = 0;
    auto btn_search_back = Button(" ◀ 返回播放列表 ", [&] { g_viewSel = 0; });
    auto search_panel = Container::Vertical({
        search_input,
        btn_search_back,
    });
    std::vector<Component> views = {layout, settings_panel, search_panel};
    auto view_container = Container::Tab(views, &g_viewSel);

    // UI 刷新 tick
    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            screen.PostEvent(Event::Custom);
        }
    });

    // 双击播放: 记录上次点击时间与行, 300ms 内同位置再点则播放
    auto last_click = std::chrono::steady_clock::now();
    int last_click_y = -1;

    // 快捷键 + 刷新事件
    auto component = CatchEvent(view_container, [&](Event event) {
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Pressed) {
            auto now = std::chrono::steady_clock::now();
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click).count();
            int y = event.mouse().y;
            if (dt < 300 && y == last_click_y && g_hasPlaylist) {
                if (g_cb_play) g_cb_play(g_selected);
                last_click_y = -1;  // 防三击连播
                return true;
            }
            last_click = now;
            last_click_y = y;
            return false;  // 单击: 交给 Menu 选中条目
        }
        if (event == Event::Custom) {
            // 睡眠定时器: 到点暂停 + 提示
            if (g_sleepActive && std::chrono::steady_clock::now() >= g_sleepEnd) {
                g_sleepActive = false;
                g_sleepMin = 0;
                g_sleepMsg = "睡眠定时器到点,已暂停";
                g_sleepMsgMs = nowMs() + 2500;
                g_player.pause();
            }
            // 音量滑块同步到播放器(淡入淡出进行中不打断)
            if (g_player.state() == Player::State::Playing && !g_player.isFading() &&
                std::abs(g_uiVolume - static_cast<float>(g_player.volume())) > 0.5f) {
                g_player.setVolume(static_cast<int>(g_uiVolume));
            }
            // 切歌检测: 加载对应歌词 + 封面
            size_t ci = g_player.currentIndex();
            if (ci != lrcTrackIndex || lrcPending) {
                lrcTrackIndex = ci;
                loadLrcFor(ci);
                CoverLoadFor(ci, g_files);
            }
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            g_autoPlay = g_autoplaySel == 1;
            g_showLrcPanel = g_showlrcSel == 0;
            g_viewSel = g_viewSel == 0 ? 1 : 0;
            showHistory = false;
            showHelp = false;
            return true;
        }
        if (event == Event::Character('/')) {
            if (g_viewSel == 2) {
                g_viewSel = 0;
            } else if (!g_files.empty()) {
                searchQuery.clear();
                searchSel = 0;
                g_viewSel = 2;
            }
            return true;
        }
        if (event == Event::Character('?') ) {
            showHelp = !showHelp;
            return true;
        }
        // T: 睡眠定时器(循环 30/60/90/关, 大小写通吃)
        if (event == Event::Character('t') || event == Event::Character('T')) {
            if (g_sleepActive) {
                g_sleepActive = false;
                g_sleepMin = 0;
                g_sleepMsg = "睡眠定时器已取消";
            } else {
                if (g_sleepMin == 0) g_sleepMin = 30;
                else if (g_sleepMin == 30) g_sleepMin = 60;
                else if (g_sleepMin == 60) g_sleepMin = 90;
                else g_sleepMin = 30;
                g_sleepEnd = std::chrono::steady_clock::now() + std::chrono::minutes(g_sleepMin);
                g_sleepActive = true;
                g_sleepMsg = "睡眠定时器 " + std::to_string(g_sleepMin) + " 分钟";
            }
            g_sleepMsgMs = nowMs() + 2500;
            return true;
        }
        // i: 歌曲详情弹窗
        if (event == Event::Character('i') || event == Event::Character('I')) {
            g_showDetail = !g_showDetail;
            return true;
        }
        if (showHelp && event == Event::Escape) {
            showHelp = false;
            return true;
        }
        if (g_showDetail && event == Event::Escape) {
            g_showDetail = false;
            return true;
        }
        // ---- 播放队列: 在队列视图内按 a = 把当前曲目追加 ----
        if (event == Event::Character('a') && showQueue) {
            if (g_selected >= 0 && static_cast<size_t>(g_selected) < g_files.size())
                queueAdd(static_cast<size_t>(g_selected));
            return true;
        }
        // ---- 歌单: 首页 A/a 弹选择层加入 / F4 打开管理 ----
        if (showPlAdd) {
            if (event == Event::ArrowUp && plAddSel > 0) { plAddSel--; return true; }
            if (event == Event::ArrowDown && plAddSel + 1 < static_cast<int>(playlists.size())) { plAddSel++; return true; }
            if (event == Event::Escape) { showPlAdd = false; return true; }
            if (event == Event::Return) {
                if (!playlists.empty()) plAddTo(plAddSel);
                return true;
            }
        }
        if ((event == Event::Character('a') || event == Event::Character('A')) && !showQueue && !showPlaylists && !showPlAdd) {
            // 首页加入歌单: 弹出选择层(无歌单时自动建一个并加入)
            if (playlists.empty()) {
                playlists.push_back({"歌单1", {}});
                plSel = 0;
                savePlaylists();
                addToPlaylist(0);
            } else {
                plAddSel = 0;
                showPlAdd = true;
            }
            return true;
        }
        if (event == Event::F4) {
            showPlaylists = !showPlaylists;
            plSel = 0;
            showPlRename = false;
            return true;
        }
        if (showPlaylists) {
            // 歌单管理(折叠式): ↑↓ 导航, Enter 展开/折叠/播放, r 重命名, n 新建,
            // x 删除歌单, d 移除曲目, C 清空展开歌单, Esc 关闭
            if (showPlRename) {
                // 手动输入模式: 可打印字符追加, Backspace 退格, 回车确认, Esc 取消
                if (event == Event::Return) {
                    // 用行模型解析当前行对应的歌单索引(plSel 是行索引, 展开时会偏移)
                    int plIdx, trIdx;
                    if (!plRenameBuf.empty() && plRowOf(plSel, plIdx, trIdx) && trIdx < 0) {
                        playlists[static_cast<size_t>(plIdx)].name = plRenameBuf;
                        savePlaylists();
                    }
                    showPlRename = false;
                    plRenameBuf.clear();
                    return true;
                }
                if (event == Event::Escape) {
                    showPlRename = false;
                    plRenameBuf.clear();
                    return true;
                }
                if (event == Event::Backspace && !plRenameBuf.empty()) {
                    size_t n = plRenameBuf.size();
                    while (n > 0 && (static_cast<unsigned char>(plRenameBuf[n - 1]) & 0xC0) == 0x80) n--;
                    if (n > 0) n--;
                    plRenameBuf.resize(n);
                    return true;
                }
                if (event.is_character()) {
                    plRenameBuf += event.character();
                    return true;
                }
            }
            if (event == Event::ArrowUp && plSel > 0) { plSel--; return true; }
            if (event == Event::ArrowDown && plSel + 1 < plRowCount()) { plSel++; return true; }
            if (event == Event::Escape) { showPlaylists = false; return true; }
            // P: 播放整个歌单(替换播放列表, 回到主界面)
            if (event == Event::Character('p') || event == Event::Character('P')) {
                int plIdx, trIdx;
                if (plRowOf(plSel, plIdx, trIdx) && trIdx < 0) {
                    auto& pl = playlists[static_cast<size_t>(plIdx)];
                    std::vector<std::string> plFiles;
                    for (auto& t : pl.tracks) {
                        if (std::filesystem::is_regular_file(t)) plFiles.push_back(t);
                    }
                    if (!plFiles.empty() && g_cb_play) {
                        g_files = plFiles;
                        g_hasPlaylist = true;
                        g_player.setPlaylist(g_files);
                        g_cb_play(0);
                        g_selected = 0;
                        g_history.insert(g_history.begin(), g_files[0]);
                        if (g_history.size() > 100) g_history.pop_back();
                        showPlaylists = false;
                    }
                }
                return true;
            }
            if (event == Event::Return) {
                int plIdx, trIdx;
                if (plRowOf(plSel, plIdx, trIdx)) {
                    if (trIdx < 0) {
                        // 歌单行: 展开/折叠
                        plExpanded = (plExpanded == plIdx) ? -1 : plIdx;
                    } else {
                        // 曲目行: 播放该曲目, 回到主界面
                        auto& pl = playlists[static_cast<size_t>(plIdx)];
                        if (static_cast<size_t>(trIdx) < pl.tracks.size() &&
                            std::filesystem::is_regular_file(pl.tracks[static_cast<size_t>(trIdx)])) {
                            auto it = std::find(g_files.begin(), g_files.end(), pl.tracks[static_cast<size_t>(trIdx)]);
                            if (it != g_files.end() && g_cb_play) {
                                size_t idx = static_cast<size_t>(it - g_files.begin());
                                g_cb_play(static_cast<int>(idx));
                                g_selected = static_cast<int>(idx);
                                g_history.insert(g_history.begin(), g_files[idx]);
                                if (g_history.size() > 100) g_history.pop_back();
                            }
                        }
                        showPlaylists = false;
                    }
                }
                return true;
            }
            if (event == Event::Character('r') || event == Event::Character('R')) {
                int plIdx, trIdx;
                if (plRowOf(plSel, plIdx, trIdx) && trIdx < 0) {
                    plRenameBuf = playlists[static_cast<size_t>(plIdx)].name;
                    showPlRename = true;
                }
                return true;
            }
            if (event == Event::Character('x') || event == Event::Character('X')) {
                int plIdx, trIdx;
                if (plRowOf(plSel, plIdx, trIdx) && trIdx < 0) {
                    playlists.erase(playlists.begin() + plIdx);
                    if (plExpanded == plIdx) plExpanded = -1;
                    if (plSel >= plRowCount()) plSel = plRowCount() - 1;
                    if (plSel < 0) plSel = 0;
                    savePlaylists();
                }
                return true;
            }
            if (event == Event::Character('d') || event == Event::Character('D')) {
                int plIdx, trIdx;
                if (plRowOf(plSel, plIdx, trIdx)) {
                    if (trIdx >= 0) plRemoveTrack(plIdx, trIdx);
                    else plRemoveTrack(plIdx, -1);
                }
                return true;
            }
            if (event == Event::Character('c') || event == Event::Character('C')) {
                if (plExpanded >= 0 && plExpanded < static_cast<int>(playlists.size())) {
                    playlists[static_cast<size_t>(plExpanded)].tracks.clear();
                    savePlaylists();
                }
                return true;
            }
            if (event == Event::Character('n') || event == Event::Character('N')) {
                std::string name = "歌单" + std::to_string(playlists.size() + 1);
                playlists.push_back({name, {}});
                savePlaylists();
                plSel = plRowCount() - 1;
                return true;
            }
        }
        if (event == Event::Character('i') || event == Event::Character('I')) {
            if (g_selected >= 0 && static_cast<size_t>(g_selected) < g_files.size())
                queueInsert(static_cast<size_t>(g_selected));
            return true;
        }
        if (event == Event::Character('d') || event == Event::Character('D')) {
            if (showQueue) { queueRemove(); return true; }
            if (event == Event::Character('D')) { queue.clear(); return true; }
        }
        if (event == Event::Character('j') || event == Event::Character('J')) {
            if (showQueue && queueSel > 0) {
                std::swap(queue[static_cast<size_t>(queueSel)], queue[static_cast<size_t>(queueSel - 1)]);
                queueSel--;
            }
            return true;
        }
        if (event == Event::Character('k') || event == Event::Character('K')) {
            if (showQueue && queueSel + 1 < static_cast<int>(queue.size())) {
                std::swap(queue[static_cast<size_t>(queueSel)], queue[static_cast<size_t>(queueSel + 1)]);
                queueSel++;
            }
            return true;
        }
        if ((event == Event::Character('q') || event == Event::Character('Q')) && showQueue) {
            showQueue = false;
            return true;
        }
        if (event == Event::Tab) {
            showQueue = !showQueue;
            return true;
        }
        if (showQueue) {
            // 队列导航: ↑↓ 选择
            if (event == Event::ArrowUp && queueSel > 0) { queueSel--; return true; }
            if (event == Event::ArrowDown && queueSel + 1 < static_cast<int>(queue.size())) { queueSel++; return true; }
        }
        if (g_viewSel == 2) {
            // 搜索视图: ↑↓ 选择, 回车播放, Esc 关闭
            if (event == Event::ArrowUp && searchSel > 0) { searchSel--; return true; }
            if (event == Event::ArrowDown && searchSel + 1 < static_cast<int>(searchResults.size())) { searchSel++; return true; }
            if (event == Event::Escape) { g_viewSel = 0; return true; }
            if (event == Event::Return && !searchResults.empty() &&
                searchSel >= 0 && static_cast<size_t>(searchSel) < searchResults.size()) {
                if (g_cb_play) {
                    auto it = std::find(g_files.begin(), g_files.end(), searchResults[static_cast<size_t>(searchSel)]);
                    if (it != g_files.end()) {
                        size_t idx = static_cast<size_t>(it - g_files.begin());
                        g_cb_play(static_cast<int>(idx));
                        g_selected = static_cast<int>(idx);
                        g_history.insert(g_history.begin(), g_files[idx]);
                        if (g_history.size() > 100) g_history.pop_back();
                    }
                }
                g_viewSel = 0;
                return true;
            }
        }
        if (event == Event::Character('h') || event == Event::Character('H')) {
            if (!g_history.empty()) {
                showHistory = !showHistory;
                histSel = 0;
            }
            return true;
        }
        if (event == Event::Character('\x0c') || event == Event::Character('\x0C')) {  // Ctrl+L
            lrcSource = (lrcSource + 1) % 3;
            loadLrcFor(g_player.currentIndex());
            return true;
        }
        if (event == Event::Return) {
            if (showQueue) {
                // 队列内回车: 播放选中队列项
                if (g_cb_play && !queue.empty() && queueSel >= 0 &&
                    static_cast<size_t>(queueSel) < queue.size()) {
                    auto it = std::find(g_files.begin(), g_files.end(), queue[static_cast<size_t>(queueSel)]);
                    if (it != g_files.end()) {
                        size_t idx = static_cast<size_t>(it - g_files.begin());
                        g_cb_play(static_cast<int>(idx));
                        g_selected = static_cast<int>(idx);
                        g_history.insert(g_history.begin(), g_files[idx]);
                        if (g_history.size() > 100) g_history.pop_back();
                    }
                }
                return true;
            }
            if (showHistory) {
                // 历史弹窗内回车: 播放选中项
                if (g_cb_play && g_hasPlaylist && !g_history.empty() &&
                    histSel >= 0 && static_cast<size_t>(histSel) < g_history.size()) {
                    auto it = std::find(g_files.begin(), g_files.end(),
                                        g_history[static_cast<size_t>(histSel)]);
                    if (it != g_files.end()) {
                        size_t idx = static_cast<size_t>(it - g_files.begin());
                        g_cb_play(static_cast<int>(idx));
                        g_selected = static_cast<int>(idx);
                        g_history.erase(std::remove(g_history.begin(), g_history.end(),
                                                    g_history[static_cast<size_t>(histSel)]),
                                        g_history.end());
                        g_history.insert(g_history.begin(), g_files[idx]);
                        if (g_history.size() > 100) g_history.pop_back();
                    }
                }
                showHistory = false;
                return true;
            }
            if (g_cb_play && g_hasPlaylist) {
                g_cb_play(g_selected);
                if (g_selected >= 0 && static_cast<size_t>(g_selected) < g_files.size()) {
                    g_history.insert(g_history.begin(), g_files[static_cast<size_t>(g_selected)]);
                    if (g_history.size() > 100) g_history.pop_back();
                }
            }
            return true;
        }
        if (showHistory) {
            // 历史弹窗: ↑↓ 选择, Esc 关闭
            if (event == Event::ArrowUp && histSel > 0) { histSel--; return true; }
            if (event == Event::ArrowDown && histSel + 1 < static_cast<int>(g_history.size())) { histSel++; return true; }
            if (event == Event::Escape) { showHistory = false; return true; }
        }
        if (event == Event::Character(' ')) {
            if (g_cb_toggle) g_cb_toggle();
            return true;
        }
        if (event == Event::Character('m') || event == Event::Character('M')) {
            // M: 音乐库浏览器(模式切换走按钮)
            if (g_viewSel == 2) { g_viewSel = 0; return true; }
            if (showQueue) { showQueue = false; return true; }
            if (!showLib) {
                if (libArtists.empty()) buildLibArtists();
                showLib = true;
                libLevel = 0;
                libSel = 0;
            } else {
                if (libLevel > 0) {
                    libLevel--;
                    libSel = 0;
                } else {
                    showLib = false;
                }
            }
            return true;
        }
        if (showLib) {
            // 音乐库导航: ↑↓ 选择, 回车深入, Esc 返回
            if (event == Event::ArrowUp && libSel > 0) { libSel--; return true; }
            if (event == Event::ArrowDown) {
                size_t n = (libLevel == 0) ? libArtists.size() :
                           (libLevel == 1) ? libAlbums.size() : libTracks.size();
                if (libSel + 1 < static_cast<int>(n)) { libSel++; return true; }
            }
            if (event == Event::Escape) {
                if (libLevel > 0) { libLevel--; libSel = 0; }
                else showLib = false;
                return true;
            }
            if (event == Event::Return) {
                if (libLevel == 0 && !libArtists.empty() && libSel < static_cast<int>(libArtists.size())) {
                    libArtistIdx = libSel;
                    openLibArtist(libSel);
                    libLevel = 1;
                    libSel = 0;
                    return true;
                }
                if (libLevel == 1 && !libAlbums.empty() && libSel < static_cast<int>(libAlbums.size())) {
                    openLibAlbum(libArtistIdx, libSel);
                    libLevel = 2;
                    libSel = 0;
                    return true;
                }
                if (libLevel == 2 && !libTracks.empty() && libSel < static_cast<int>(libTracks.size())) {
                    size_t idx = libTracks[static_cast<size_t>(libSel)];
                    if (g_cb_play && idx < g_files.size()) {
                        g_cb_play(static_cast<int>(idx));
                        g_selected = static_cast<int>(idx);
                        g_history.insert(g_history.begin(), g_files[idx]);
                        if (g_history.size() > 100) g_history.pop_back();
                    }
                    return true;
                }
            }
        }
        if (event == Event::Character('+') || event == Event::Character('=')) {
            if (g_cb_volume) g_cb_volume(g_player.volume() + 5);
            return true;
        }
        if (event == Event::Character('-')) {
            if (g_cb_volume) g_cb_volume(g_player.volume() - 5);
            return true;
        }
        if (event == Event::ArrowLeft) {
            if (g_cb_seek) g_cb_seek(g_player.position() - 5.0);
            return true;
        }
        if (event == Event::ArrowRight) {
            if (g_cb_seek) g_cb_seek(g_player.position() + 5.0);
            return true;
        }
        if (event == Event::Character('r') || event == Event::Character('R')) {
            if (g_cb_refresh) g_cb_refresh();
            return true;
        }
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            SaveConfig();
            screen.Exit();
            return true;
        }
        return false;
    });

    // 渲染
    auto renderer = Renderer(component, [&] {
        // 歌词加载挂起时轮询(每帧检查内嵌歌词就绪, 就绪才加载——不消耗 lrcRetry,
        // 避免 60fps 重试在 openCurrent 完成前耗尽 8 次上限导致歌词丢失)
        if (lrcPending && g_player.currentIndex() == lrcTrackIndex &&
            !g_player.embeddedLyrics().empty()) {
            loadLrcFor(lrcTrackIndex);
        }
        // 播放/暂停按钮文本随状态刷新
        updatePlayPauseLabel();
        // 封面按当前终端尺寸动态生成 + 右栏宽度(termW 为外层变量, 每帧更新)
        {
            auto term = Terminal::Size();
            int tw = term.dimx > 0 ? term.dimx : 120;
            int th = term.dimy > 0 ? term.dimy : 40;
            termW = tw;
            CoverEnsureCells(tw, th);
        }
        Player::State st = g_player.state();
        std::string state_str;
        Color state_color = Color::GrayDark;
        switch (st) {
            case Player::State::Playing: state_str = "播放中"; state_color = Color::Green; break;
            case Player::State::Paused:  state_str = "已暂停"; state_color = Color::Yellow; break;
            case Player::State::Stopped: state_str = "已停止"; state_color = Color::GrayDark; break;
        }

        double pos = g_player.position();
        double dur = g_player.duration();
        float frac = dur > 0.0 ? static_cast<float>(pos / dur) : 0.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;

        std::string title = g_hasPlaylist ? BaseName(g_player.currentTitle()) : "(无播放列表)";
        std::string trackInfo = g_hasPlaylist
            ? "曲目: " + std::to_string(g_player.currentIndex() + 1) + "/" +
              std::to_string(g_files.size())
            : "曲目: -";
        std::string dirInfo = "目录: " + (g_scanDir.empty() ? "(未指定)" : g_scanDir);

        bool isFav = false;
        if (g_hasPlaylist && g_player.currentIndex() < g_files.size()) {
            isFav = IsFavorite(g_files[g_player.currentIndex()]);
        }

        auto progress_bar = vbox({
            seek_bar->Render(),  // 左侧时间数字 + 进度条(可点击/拖动 seek)
        });

        // ---- 歌单管理视图(F4, 折叠式; 内嵌左列, 歌词/封面保留) ----
        Element pl_render;
        if (showPlaylists) {
            std::vector<Element> rows;
            rows.push_back(text(" 歌单管理 (F4 关闭) ") | bold | inverted);
            rows.push_back(separator());
            if (playlists.empty()) {
                rows.push_back(text(" (无歌单: 按 N 新建, 然后首页选中曲目按 A 加入)") | dim);
            } else {
                int r = 0;
                for (size_t i = 0; i < playlists.size(); ++i) {
                    bool expanded = (plExpanded == static_cast<int>(i));
                    std::string arrow = expanded ? "▼" : "▶";
                    std::string label = " " + arrow + " " + playlists[i].name +
                                        " (" + std::to_string(playlists[i].tracks.size()) + " 首)";
                    rows.push_back(text(label) |
                        (r == plSel ? bold | color(Color::Yellow) : (expanded ? bold : dim)));
                    r++;
                    if (expanded) {
                        if (playlists[i].tracks.empty()) {
                            rows.push_back(text("     (空歌单)") |
                                (r == plSel ? bold | color(Color::Yellow) : dim));
                            r++;
                        } else {
                            for (size_t j = 0; j < playlists[i].tracks.size(); ++j) {
                                std::string tname = BaseName(playlists[i].tracks[j]);
                                rows.push_back(text("     " + std::to_string(j + 1) + ". " + tname) |
                                    (r == plSel ? bold | color(Color::Yellow) : dim));
                                r++;
                            }
                        }
                    }
                }
            }
            rows.push_back(separator());
            if (showPlRename) {
                rows.push_back(hbox({
                    text(" 重命名: ") | bold,
                    text(plRenameBuf) | flex,
                }));
                rows.push_back(text(" 回车=确认  Esc=取消") | dim);
            } else {
                rows.push_back(text(" ↑↓=选择  Enter=展开/折叠/播放  P=播放全部  R=重命名  N=新建  X=删除歌单  D=移除曲目  C=清空  Esc=关闭") | dim);
            }
            pl_render = vbox(std::move(rows)) | flex;
        }

        Element list_render;
        if (showPlaylists) {
            // 歌单管理(F4): 内嵌左列, 右侧歌词/封面保留
            list_render = pl_render;
        } else if (showQueue) {
            // 队列视图(Tab 切换)
            std::vector<Element> qrows;
            qrows.push_back(text(" 播放队列 ") | bold | inverted);
            qrows.push_back(separator());
            if (queue.empty()) {
                qrows.push_back(text(" (空队列: 选中曲目按 a 追加 / i 插入)") | dim);
            } else {
                for (size_t i = 0; i < queue.size(); ++i) {
                    std::string label = " " + std::to_string(i + 1) + ". " + BaseName(queue[i]);
                    qrows.push_back(text(label) |
                        (static_cast<int>(i) == queueSel ? bold | color(Color::Yellow) : dim));
                }
            }
            qrows.push_back(separator());
            qrows.push_back(text(" ↑↓=选择  A=追加当前曲目  I=插入  D=移除  D=清空  J/K=移动  回车=播放  Q=关闭") | dim);
            list_render = vbox(std::move(qrows)) | flex;
        } else {
            list_render = vbox({
                playlist->Render() | frame | flex,
                text(" " + std::to_string(g_files.size()) + " 个文件  (Tab=队列 a=加入)") | dim,
            });
        }

        auto info_render = vbox({
            // 歌名: 单行, 超长省略号; 无标题栏, 封面紧贴(无空行)
            text(TruncateW(title, std::max(8, termW / 2 - 6))) | bold | color(Color::Yellow),
            // 封面(歌名正下方, 占满到进度条)
            [&]() -> Element {
                if (g_coverCells.empty()) return text("");
                int cols = g_coverGenW, rows = g_coverGenH;
                std::vector<Element> rows_e;
                for (int c2 = 0; c2 < rows; ++c2) {
                    std::vector<Element> r2;
                    for (int c3 = 0; c3 < cols; ++c3) {
                        const CoverCell& cell = g_coverCells[static_cast<size_t>(c2) * cols + c3];
                        // quadrant 块字符: 实心象限=前景色, 空心象限=背景色 (2x2 像素/格)
                        r2.push_back(text(cell.ch) |
                            color(Color::RGB(cell.r1, cell.g1, cell.b1)) |
                            bgcolor(Color::RGB(cell.r2, cell.g2, cell.b2)));
                    }
                    rows_e.push_back(hbox(std::move(r2)));
                }
                return vbox(std::move(rows_e));
            }() | hcenter | flex_grow,
            progress_bar,   // 左侧时间数字 + 进度条
            volume_bar->Render(),   // 进度条与音量条之间不留空行
            // 频谱已移至底部全宽(cava 风格, 见 main_ui)
        });

        // ---- 设置视图 ----
        if (g_viewSel == 1) {
            return vbox({
                hbox({
                    text(" ⚙ Termus 设置 ") | bold | color(Color::Cyan),
                    text("S=返回") | dim,
                }),
                separator(),
                settings_panel->Render() | frame | flex,
                separator(),
                hbox({
                    text(" 自动播放: 启动时自动播放第一首    歌词对齐: 居左/居中/居右    清空历史: 删除播放历史") | dim,
                }),
                separator(),
                hbox({
                    text(" 关于: Termus (Kaula/FTXUI 版) — 灵感来自 ter-music v2.1.0 (浣软科技)") | dim,
                }),
            });
        }

        // ---- 搜索视图 ----
        if (g_viewSel == 2) {
            doSearch();
            std::vector<Element> res;
            size_t n = searchResults.size();
            for (size_t i = 0; i < n; ++i) {
                std::string label = " " + std::to_string(i + 1) + ". " + BaseName(searchResults[i]);
                res.push_back(text(label) |
                    (static_cast<int>(i) == searchSel ? bold | color(Color::Yellow) : dim));
            }
            return vbox({
                hbox({
                    text(" 🔍 搜索 ") | bold | color(Color::Cyan),
                    text(" /=返回  Esc=关闭") | dim,
                }),
                separator(),
                search_input->Render(),
                text(" 匹配 " + std::to_string(n) + " 首  (↑↓选择, 回车播放)") | dim,
                separator(),
                vbox(std::move(res)) | frame | flex,
            });
        }

        // ---- 音乐库浏览器视图(M 键) ----
        if (showLib) {
            std::vector<Element> rows;
            std::string title;
            size_t n = 0;
            if (libLevel == 0) {
                title = " 🎵 音乐库 · 艺术家 ";
                n = libArtists.size();
                for (size_t i = 0; i < n; ++i) {
                    rows.push_back(text(" " + std::to_string(i + 1) + ". " + libArtists[i]) |
                        (static_cast<int>(i) == libSel ? bold | color(Color::Yellow) : dim));
                }
            } else if (libLevel == 1) {
                title = " 🎵 " + libArtists[static_cast<size_t>(libArtistIdx)] + " · 专辑 ";
                n = libAlbums.size();
                for (size_t i = 0; i < n; ++i) {
                    rows.push_back(text(" " + std::to_string(i + 1) + ". " + libAlbums[i]) |
                        (static_cast<int>(i) == libSel ? bold | color(Color::Yellow) : dim));
                }
            } else {
                title = " 🎵 专辑曲目 ";
                n = libTracks.size();
                for (size_t i = 0; i < n; ++i) {
                    size_t idx = libTracks[i];
                    std::string name = (idx < g_files.size()) ? BaseName(g_files[idx]) : "?";
                    rows.push_back(text(" " + std::to_string(i + 1) + ". " + name) |
                        (static_cast<int>(i) == libSel ? bold | color(Color::Yellow) : dim));
                }
            }
            return vbox({
                hbox({
                    text(title) | bold | color(Color::Cyan),
                    text("  M=返回  Esc=返回  Enter=进入/播放") | dim,
                }),
                separator(),
                vbox(std::move(rows)) | frame | flex,
            });
        }

        // ---- 首页加入歌单覆盖层(A/a 键) ----
        if (showPlAdd) {
            std::vector<Element> rows;
            rows.push_back(text(" 📋 加入歌单 ") | bold | color(Color::Cyan));
            rows.push_back(separator());
            for (size_t i = 0; i < playlists.size(); ++i) {
                std::string label = " " + std::to_string(i + 1) + ". " + playlists[i].name +
                                    " (" + std::to_string(playlists[i].tracks.size()) + " 首)";
                rows.push_back(text(label) |
                    (static_cast<int>(i) == plAddSel ? bold | color(Color::Yellow) : dim));
            }
            rows.push_back(separator());
            rows.push_back(text(" ↑↓=选择  Enter=加入   Esc=取消") | dim);
            return vbox({
                vbox(std::move(rows)) | frame | flex,
            });
        }

        auto main_ui = vbox({
            // 左列=播放列表+歌词+cava频谱, 右列=内容(歌名/封面/进度/音量, 固定 1/2 宽)
            hbox({
                vbox({
                    list_render | flex,
                    separator(),
                    // 歌词块(左栏中部)
                    vbox({
                        hbox({
                            text(" 歌词[" + std::string(lrcSourceName(lrcSource)) + "] ") | bold | dim,
                            text(lrcSource == 1 ? lrcFile : "(内嵌/自动)") | dim | flex_grow,
                        }),
                        lrcRender(lrc, pos, lrcAlignSel) | flex,
                    }),
                    separator(),
                    // cava 频谱(歌词下面): 占左栏全宽(termW/2-1: 减分隔线), 柱从底生长, 底部绿顶部红
                    cavaSpectrumViz(6, termW / 2 - 1) | size(HEIGHT, EQUAL, 6),
                }) | flex,
                separator(),
                info_render | size(WIDTH, EQUAL, termW / 2),
            }) | flex,
            separator(),
            controls->Render() | center,
            separator(),
            hbox({
                text(" 状态: ") | dim,
                text(state_str) | color(state_color),
                text("   " + trackInfo) | dim,
                // 睡眠定时器状态
                [&]() -> Element {
                    if (!g_sleepMsg.empty() && nowMs() < g_sleepMsgMs) {
                        return text(" " + g_sleepMsg + " ") | color(Color::Yellow);
                    }
                    if (g_sleepActive) {
                        auto remain = std::chrono::duration_cast<std::chrono::seconds>(
                            g_sleepEnd - std::chrono::steady_clock::now()).count();
                        if (remain < 0) remain = 0;
                        char sb[32];
                        snprintf(sb, sizeof(sb), "🔒 %02d:%02d",
                                 static_cast<int>(remain / 60), static_cast<int>(remain % 60));
                        return text(std::string(" ") + sb + " ") | color(Color::Yellow);
                    }
                    return text("");
                }(),
                text("   " + dirInfo) | dim | flex_grow,
            }),
            // 快捷键提示已移除(按 ? 查看帮助弹窗), 为封面/频谱让出空间
        });

        // 播放历史弹窗(H 键, 原版样式): 居中覆盖
        if (showHistory) {
            std::vector<Element> hrows;
            size_t n = std::min<size_t>(g_history.size(), 10);
            hrows.push_back(text(" 播放历史 ") | bold | inverted);
            hrows.push_back(separator());
            for (size_t i = 0; i < n; ++i) {
                std::string label = " " + std::to_string(i + 1) + ". " + BaseName(g_history[i]);
                hrows.push_back(text(label) |
                    (static_cast<int>(i) == histSel ? bold | color(Color::Yellow) : dim));
            }
            hrows.push_back(separator());
            hrows.push_back(text(" ↑↓=选择  回车=播放  Esc=关闭") | dim);
            auto hist_box = vbox(std::move(hrows)) | border |
                size(WIDTH, EQUAL, 50) | size(HEIGHT, EQUAL, static_cast<int>(n) + 4);
            main_ui = dbox({
                main_ui,
                hist_box | center,
            });
        }

        // 歌曲详情弹窗(i 键): 当前曲目信息, 居中覆盖
        if (g_showDetail) {
            std::vector<Element> drows;
            drows.push_back(text(" 歌曲信息 ") | bold | inverted);
            drows.push_back(separator());
            size_t ci = g_player.currentIndex();
            std::string title = (ci < g_files.size()) ? BaseName(g_files[ci]) : "-";
            double dur = g_player.duration();
            char dbuf[64];
            snprintf(dbuf, sizeof(dbuf), "%d:%02d", static_cast<int>(dur) / 60,
                     static_cast<int>(dur) % 60);
            drows.push_back(text(" 标题: " + title) | dim);
            drows.push_back(text(" 时长: " + std::string(dbuf)) | dim);
            drows.push_back(text(" 编码: " + g_player.codecName()) | dim);
            drows.push_back(text(" 采样率: " + std::to_string(g_player.sampleRate()) + " Hz") | dim);
            drows.push_back(text(" 位深: " + std::to_string(g_player.bitDepth()) + " bit") | dim);
            drows.push_back(text(" 码率: " + std::to_string(g_player.bitRate() / 1000) + " kbps") | dim);
            drows.push_back(text(" 路径: " + (ci < g_files.size() ? g_files[ci] : std::string("-"))) | dim);
            drows.push_back(separator());
            drows.push_back(text(" Esc=关闭") | dim);
            auto detail_box = vbox(std::move(drows)) | border |
                size(WIDTH, EQUAL, 64);
            main_ui = dbox({
                main_ui,
                detail_box | center,
            });
        }

        // 帮助弹窗(? 键): 快捷键说明, 居中覆盖
        if (showHelp) {
            std::vector<Element> hlp;
            for (auto& line : helpLines) {
                hlp.push_back(text(line) | dim);
            }
            auto help_box = vbox(std::move(hlp)) | border |
                size(WIDTH, EQUAL, 50);
            main_ui = dbox({
                main_ui,
                help_box | center,
            });
        }
        return main_ui;
    });

    screen.Loop(renderer);

    // 退出: 保存配置(含上次播放位置)
    if (g_player.currentIndex() >= 0 && static_cast<size_t>(g_player.currentIndex()) < g_files.size()) {
        g_lastTrack = g_files[static_cast<size_t>(g_player.currentIndex())];
        g_lastPos = g_player.position();
    }
    SaveConfig();

    running = false;
    if (ticker.joinable()) ticker.join();
}
