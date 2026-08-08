// termus_bridge.cpp — termus 桥接层实现
// FTXUI UI + Player(FFmpeg 解码) + waveOut 输出的 C 封装。
// 事件通过注册的 C 函数指针回调给 Kaula 代码处理。
#include "termus_bridge.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "src/player.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

namespace {

Player g_player;
std::vector<std::string> g_files;
std::vector<std::string> g_favorites;
std::string g_scanDir;
bool g_hasPlaylist = false;

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

std::string FormatTime(double sec) {
    if (sec < 0) sec = 0;
    auto total = static_cast<int>(sec);
    return std::to_string(total / 60) + ":" +
           (total % 60 < 10 ? "0" : "") + std::to_string(total % 60);
}

std::string BaseName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string FormatBitrate(int64_t br) {
    if (br <= 0) return "?";
    if (br >= 1000000) return std::to_string(br / 1000000) + "." +
                               std::to_string((br % 1000000) / 100000) + "Mbps";
    return std::to_string(br / 1000) + "kbps";
}

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

    auto screen = ScreenInteractive::Fullscreen();
    g_screen = &screen;

    // 播放列表条目(随 refresh 更新)
    auto entries = std::make_shared<std::vector<std::string>>();
    for (size_t i = 0; i < g_files.size(); ++i) {
        entries->push_back("  " + std::to_string(i + 1) + ". " + BaseName(g_files[i]));
    }

    auto playlist = Menu(entries.get(), &g_selected);
    auto volume_slider = Slider(" 音量", &g_uiVolume, 0.0f, 100.0f, 1.0f);

    auto btn_play = Button(" ▶ 播放 ", [&] { if (g_cb_play) g_cb_play(g_selected); });
    auto btn_pause = Button(" ⏸ 播放/暂停 ", [&] { if (g_cb_toggle) g_cb_toggle(); });
    auto btn_prev = Button(" ⏮ 上一首 ", [&] { if (g_cb_prev) g_cb_prev(); });
    auto btn_next = Button(" ⏭ 下一首 ", [&] { if (g_cb_next) g_cb_next(); });
    auto btn_stop = Button(" ⏹ 停止 ", [&] { if (g_cb_stop) g_cb_stop(); });
    auto btn_mode = Button(" 🔁 模式 ", [&] { if (g_cb_mode) g_cb_mode(); });
    auto btn_speed = Button(" ⏩ 速度 ", [&] { if (g_cb_speed) g_cb_speed(); });
    auto btn_eq = Button(" 🎛 EQ ", [&] { if (g_cb_eq) g_cb_eq(); });
    auto btn_eq_preset = Button(" 🎚 预设 ", [&] { if (g_cb_eq_preset) g_cb_eq_preset(); });
    auto btn_refresh = Button(" 🔄 刷新 ", [&] { if (g_cb_refresh) g_cb_refresh(); });
    auto btn_fav = Button(" ♥ 收藏 ", [&] { if (g_cb_fav) g_cb_fav(); });
    auto btn_exit = Button(" ✕ 退出 ", [&] { screen.Exit(); });

    auto controls = Container::Horizontal({
        btn_play, btn_pause, btn_prev, btn_next, btn_stop, btn_mode,
        btn_speed, btn_eq, btn_eq_preset, btn_refresh, btn_fav, btn_exit,
    });
    auto right_panel = Container::Vertical({volume_slider});
    auto layout = Container::Vertical({
        Container::Horizontal({playlist, right_panel}),
        controls,
    });

    // UI 刷新 tick
    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            screen.PostEvent(Event::Custom);
        }
    });

    // 快捷键 + 刷新事件
    auto component = CatchEvent(layout, [&](Event event) {
        if (event == Event::Custom) {
            // 音量滑块同步到播放器
            if (std::abs(g_uiVolume - static_cast<float>(g_player.volume())) > 0.5f) {
                g_player.setVolume(static_cast<int>(g_uiVolume));
            }
            return true;
        }
        if (event == Event::Return) {
            if (g_cb_play && g_hasPlaylist) g_cb_play(g_selected);
            return true;
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
            screen.Exit();
            return true;
        }
        return false;
    });

    // 渲染
    auto renderer = Renderer(component, [&] {
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
            text(title) | bold | color(Color::Yellow) | flex_grow,
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
            text(" 收藏: " + std::string(isFav ? "♥ 已收藏" : "♡ 未收藏") +
                 "   共 " + std::to_string(g_favorites.size()) + " 首") | dim,
            text(""),
        });

        return vbox({
            hbox({
                text(" ♫ Termus ") | bold | color(Color::Cyan),
                text("Kaula + FTXUI + FFmpeg + waveOut") | dim,
            }),
            separator(),
            hbox({
                list_render | flex,
                separator(),
                info_render | flex_grow,
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
                text(" 音频: " + std::string(g_player.audioOk() ? "开" : "关(静默, waveOut初始化失败)")) |
                    color(g_player.audioOk() ? Color::Green : Color::Red),
                text("   " + std::string(g_hasPlaylist ? std::to_string(g_files.size()) : "0") + " 个文件") | dim | flex_grow,
            }),
            hbox({
                text(" 回车=播放  空格=播放/暂停  ←/→=±5秒  +/-=音量  M=模式  R=刷新  Q=退出") | dim,
            }),
        });
    });

    screen.Loop(renderer);

    running = false;
    if (ticker.joinable()) ticker.join();
}
