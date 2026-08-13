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
#include <iostream>
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

// 设置项(持久化)
bool g_autoPlay = false;
bool g_showLrcPanel = true;
int g_autoplaySel = 0;
int g_showlrcSel = 0;

// 视图: 0=主界面 1=设置
int g_viewSel = 0;

// ---- 专辑封面(切歌解码缓存 + 渲染时动态重采样, 同 C++ 版) ----
CoverImage g_coverImg;                    // 解码缓存(重操作, 切歌时做一次)
std::vector<CoverCell> g_coverCells;      // 当前尺寸的色块
int g_coverGenW = 0, g_coverGenH = 0;     // 上次生成时的终端尺寸
size_t g_coverTrackIdx = SIZE_MAX;        // 已解码的曲目

// 切歌时调用: 解码封面缓存(轻量封装)
void CoverLoadFor(size_t idx, const std::vector<std::string>& files) {
    g_coverImg = CoverImage();
    g_coverCells.clear();
    g_coverGenW = g_coverGenH = 0;
    g_coverTrackIdx = idx;
    if (idx >= files.size()) return;
    g_coverImg = coverDecode(files[idx]);
}

// 渲染时调用: 按终端尺寸动态生成色块(尺寸变化才重算)
void CoverEnsureCells(int termW, int termH) {
    int wantCols = termW * 3 / 10;                  // 封面宽 = 终端宽*3/10
    int wantRows = std::min(wantCols / 2, termH / 2);  // 高 = 宽/2, 不超屏高 1/2
    if (wantCols < 16) wantCols = 16;
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

// ---- LRC 歌词 ----

struct LrcLine {
    double time = 0.0;
    std::string text;
};

std::vector<LrcLine> ParseLrcText(const std::string& content) {
    std::vector<LrcLine> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        auto lb = line.find('[');
        if (lb == std::string::npos) continue;
        auto rb = line.find(']', lb);
        if (rb == std::string::npos) continue;
        std::string tag = line.substr(lb + 1, rb - lb - 1);
        if (tag.empty()) continue;
        int m = std::atoi(tag.c_str());
        auto colon = tag.find(':');
        int s = colon != std::string::npos ? std::atoi(tag.c_str() + colon + 1) : 0;
        LrcLine l;
        l.time = static_cast<double>(m) * 60.0 + static_cast<double>(s);
        l.text = line.substr(rb + 1);
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const LrcLine& a, const LrcLine& b) { return a.time < b.time; });
    return out;
}

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
    return dir + "/termus.json";
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
    std::ifstream f(ConfigPath());
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
        out = content.compare(p + 1, 4, "true") == 0;
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
        while (static_cast<int>(d) > 3) d -= 4;
        for (int i = 0; i < static_cast<int>(d); ++i) g_player.cycleMode();
    }
    if (findNum("speed", d)) g_player.setSpeed(static_cast<float>(d));
    bool b = false;
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
}

// ---- 频谱可视化 ----

Element spectrumViz() {
    static const char* glyphs[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    std::vector<Element> cols;
    float levels[Player::kSpectrumBands];
    g_player.spectrumLevels(levels, Player::kSpectrumBands);
    for (int b = 0; b < Player::kSpectrumBands; ++b) {
        int units = static_cast<int>(levels[b] * 8.0f + 0.5f);
        if (units < 0) units = 0;
        if (units > 8) units = 8;
        Color c = b < 4 ? Color::Cyan : (b < 8 ? Color::Green : Color::Magenta);
        cols.push_back(text(glyphs[units]) | color(units > 0 ? c : Color::GrayDark));
    }
    return hbox(cols);
}

// 歌词渲染(高亮当前行)
Element lrcRender(const std::vector<LrcLine>& lrc, double pos) {
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
        if (i == idx) {
            lines.push_back(text("▶ " + lrc[static_cast<size_t>(i)].text) | bold | color(Color::Cyan));
        } else {
            lines.push_back(text("  " + lrc[static_cast<size_t>(i)].text) | dim);
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

// EQ 预设(10 段, dB)
const char* kEqPresetNames[] = {"Flat", "Classical", "Dance", "Pop",
                                "Rock", "Jazz", "Bass+", "Treble+"};
constexpr int kEqPresetCount = 8;
const float kEqPresets[kEqPresetCount][kEqBands] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {4, 3, 2, 1, 0, -1, -1, 0, 2, 3},
    {6, 4, 1, 0, 0, -1, -2, -3, -2, 0},
    {-1, 1, 3, 4, 2, 0, -1, -1, 2, 3},
    {5, 4, 2, -1, -2, -3, -2, -1, 1, 2},
    {3, 2, 1, 2, -1, -1, 0, 1, 2, 3},
    {6, 5, 3, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 3, 5, 6},
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
        g_player.play(0);
        g_history.insert(g_history.begin(), g_files[0]);
        if (g_history.size() > 100) g_history.pop_back();
    }

    auto screen = ScreenInteractive::Fullscreen();
    g_screen = &screen;

    // 歌词状态
    std::vector<LrcLine> lrc;
    std::string lrcFile;
    size_t lrcTrackIndex = SIZE_MAX;
    int lrcSource = 0;  // 0=自动 1=外置 2=内嵌
    // 播放历史弹窗(H 键, 原版样式): 最近 10 首, 可上下选择回车播放
    bool showHistory = false;
    int histSel = 0;

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
        for (auto& f : g_files) {
            std::string name = BaseName(f);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find(q) != std::string::npos) searchResults.push_back(f);
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
        bool haveEmb = !emb.empty();
        std::vector<LrcLine> ext = ParseLrcFile(lrcPath);
        if (lrcSource == 1) {
            lrc = ext;
        } else {
            lrc = haveEmb ? ParseLrcText(emb) : ext;
        }
    };

    // 播放列表条目: Menu 直接引用全局 g_entries (refresh 时重建, UI 即更新)
    auto playlist = Menu(&g_entries, &g_selected);
    auto volume_slider = Slider(" 音量", &g_uiVolume, 0.0f, 100.0f, 1.0f);

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
    auto btn_eq = Button(" 🎛 EQ ", [&] { if (g_cb_eq) g_cb_eq(); });
    auto btn_eq_preset = Button(" 🎚 预设 ", [&] { if (g_cb_eq_preset) g_cb_eq_preset(); });
    auto btn_refresh = Button(" 🔄 刷新 ", [&] { if (g_cb_refresh) g_cb_refresh(); });
    auto btn_fav = Button(" ♥ 收藏 ", [&] { if (g_cb_fav) g_cb_fav(); });
    auto btn_exit = Button(" ✕ 退出 ", [&] { SaveConfig(); screen.Exit(); });

    auto controls = Container::Horizontal({
        btn_play_pause, btn_prev, btn_next, btn_stop, btn_mode,
        btn_speed, btn_eq, btn_eq_preset, btn_refresh, btn_fav, btn_exit,
    });
    auto right_panel = Container::Vertical({volume_slider});
    auto layout = Container::Vertical({
        Container::Horizontal({playlist, right_panel}),
        controls,
    });

    // ---- 设置页组件 ----
    std::vector<std::string> autoplay_entries = {"关", "开"};
    std::vector<std::string> showlrc_entries = {"开", "关"};
    auto toggle_autoplay = Toggle(autoplay_entries, &g_autoplaySel);
    auto toggle_showlrc = Toggle(showlrc_entries, &g_showlrcSel);
    auto btn_clear_hist = Button(" 清空历史 ", [&] {
        g_history.clear();
    });
    auto btn_settings_back = Button(" ◀ 返回主界面 ", [&] { g_viewSel = 0; });
    auto settings_panel = Container::Vertical({
        toggle_autoplay,
        toggle_showlrc,
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
            // 音量滑块同步到播放器
            if (std::abs(g_uiVolume - static_cast<float>(g_player.volume())) > 0.5f) {
                g_player.setVolume(static_cast<int>(g_uiVolume));
            }
            // 切歌检测: 加载对应歌词 + 封面
            size_t ci = g_player.currentIndex();
            if (ci != lrcTrackIndex) {
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
        if (showHelp && event == Event::Escape) {
            showHelp = false;
            return true;
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
            if (g_cb_mode) g_cb_mode();
            return true;
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
        // 播放/暂停按钮文本随状态刷新
        updatePlayPauseLabel();
        // 封面按当前终端尺寸动态生成 + 右栏宽度
        int termW = 120;
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
            text("  " + FormatTime(pos) + " / " + FormatTime(dur)) | dim,
            hbox({text(" "), gauge(frac) | color(Color::Cyan) | flex, text(" ")}),
        });

        auto list_render = vbox({
            text(" 播放列表 ") | bold | inverted,
            separator(),
            playlist->Render() | frame | flex,
            text(" " + std::to_string(g_files.size()) + " 个文件") | dim,
        });

        auto info_render = vbox({
            text(" 正在播放 ") | bold | inverted,
            separator(),
            // 歌名: paragraph 支持长歌名分两行
            paragraph(title) | bold | color(Color::Yellow) | flex_grow,
            // 封面(歌曲名下方, 进度条上方; half-block ▀, 无边框, 居中)
            [&]() -> Element {
                if (g_coverCells.empty()) return text("");
                int cols = g_coverGenW, rows = g_coverGenH;
                std::vector<Element> rows_e;
                for (int c2 = 0; c2 < rows; ++c2) {
                    std::vector<Element> r2;
                    for (int c3 = 0; c3 < cols; ++c3) {
                        const CoverCell& cell = g_coverCells[static_cast<size_t>(c2) * cols + c3];
                        // ▀ (U+2580): 上半块实心, 前景色=上像素, 背景色=下像素, 无缝
                        r2.push_back(text("▀") |
                            color(Color::RGB(cell.r1, cell.g1, cell.b1)) |
                            bgcolor(Color::RGB(cell.r2, cell.g2, cell.b2)));
                    }
                    rows_e.push_back(hbox(std::move(r2)));
                }
                return vbox(std::move(rows_e));
            }() | center,
            text(""),
            progress_bar,
            text(""),
            volume_slider->Render(),
            text(" 音量: " + std::to_string(g_player.volume()) + "%") | dim,
            text(""),
            text(" 模式: " + std::string(g_player.modeName()) +
                 "   速度: " + std::to_string(g_player.speed()).substr(0, 4) + "x") | dim,
            text(" 编码: " + g_player.codecName() + " | " +
                 std::to_string(g_player.sampleRate()) + "Hz | " +
                 std::to_string(g_player.bitDepth()) + "bit | " +
                 FormatBitrate(g_player.bitRate())) | dim,
            text(""),
            text(" EQ: " + std::string(g_player.eqEnabled() ? "开" : "关") +
                 "   预设: " + kEqPresetNames[g_eqPresetIdx]) |
                color(g_player.eqEnabled() ? Color::Green : Color::GrayDark),
            text(" 频谱:") | dim,
            spectrumViz(),
            text(" 收藏: " + std::string(isFav ? "♥ 已收藏" : "♡ 未收藏") +
                 "   历史: " + std::to_string(g_history.size()) + " 首 (H 查看)") | dim,
            text(""),
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
                    text(" 自动播放: 启动时自动播放第一首    显示歌词: 主界面歌词面板开关    清空历史: 删除播放历史") | dim,
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

        auto main_ui = vbox({
            hbox({
                text(" ♫ Termus ") | bold | color(Color::Cyan),
                text("Kaula + FTXUI + FFmpeg + waveOut") | dim,
            }),
            separator(),
            // 两列布局(同 C++ 版): 左列=列表+歌词(flex), 右列=信息(固定 2/5 宽)
            hbox({
                vbox({
                    list_render | flex,
                    separator(),
                    // 歌词块(左列下方)
                    g_showLrcPanel
                        ? vbox({
                              hbox({
                                  text(" 歌词[" + std::string(lrcSourceName(lrcSource)) + "] ") | bold | dim,
                                  text(lrcSource == 1 ? lrcFile : "(内嵌/自动)") | dim | flex_grow,
                              }),
                              lrcRender(lrc, pos) | flex,
                          })
                        : text(" 歌词面板已关闭 (S 进入设置开启)") | dim | flex,
                }) | flex,
                separator(),
                // 右栏 = 终端宽 * 2/5 (动态)
                info_render | size(WIDTH, EQUAL, termW * 2 / 5),
            }) | flex,
            separator(),
            controls->Render() | center,
            separator(),
            hbox({
                text(" 状态: ") | dim,
                text(state_str) | color(state_color),
                text("   " + trackInfo) | dim,
                text("   " + dirInfo) | dim | flex_grow,
            }),
            hbox({
                text(" 回车=播放  空格=播放/暂停  ←/→=±5秒  +/-=音量  M=模式  R=刷新  S=设置  H=历史  /=搜索  ?=帮助  Ctrl+L=歌词  Q=退出") | dim,
            }),
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

    // 退出: 保存配置
    SaveConfig();

    running = false;
    if (ticker.joinable()) ticker.join();
}
