// termus_bridge.h — termus 的 C 桥接层 API
//
// termus = TERMinal + MUSic: Kaula 编写的终端音乐播放器示例。
// 桥接层封装三块底层:
//   - FTXUI    (C++ UI 库: 菜单/按钮/滑块/布局)
//   - FFmpeg   (C 库: 音频解码/变速滤镜)
//   - waveOut  (Windows 系统 API: 音频输出)
// Kaula 侧通过 extern fn 调用本头文件声明的函数, 并通过 C ABI
// 回调函数指针把事件处理逻辑交回 Kaula 代码。
//
// 构建: 见 build_termus.sh
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 播放器控制 ====================

// 播放第 idx 首 (0-based)。idx 越界时自动取模。
void termus_play(int64_t idx);
// 播放/暂停切换
void termus_toggle(void);
void termus_stop(void);
void termus_next(void);
void termus_prev(void);
// 循环播放模式: 顺序 -> 列表循环 -> 单曲循环 -> 随机
void termus_cycle_mode(void);
// 快进/快退
void termus_seek(double sec);
void termus_set_volume(int64_t pct);
// 变速循环: 0.75x -> 1x -> 1.25x -> 1.5x -> 2x -> 3x
void termus_cycle_speed(void);
// 均衡器开关
void termus_set_eq_enabled(int64_t on);
// EQ 预设循环 (Flat/Classical/Dance/Pop/Rock/Jazz/Bass+/Treble+)
void termus_cycle_eq_preset(void);

// ==================== 状态查询 ====================

// 播放状态: 0=停止 1=播放中 2=已暂停
int64_t termus_state(void);
double termus_position(void);   // 秒
double termus_duration(void);   // 秒
int64_t termus_current_index(void);
// 以下返回的字符串指向桥接层内部缓冲, 仅在下一次调用前有效
const char* termus_title(void);          // 当前曲目标题(文件名)
const char* termus_mode_name(void);
int64_t termus_volume(void);
double termus_speed(void);
int64_t termus_eq_enabled(void);
const char* termus_eq_preset_name(void);
const char* termus_codec_info(void);     // 如 "AAC | 44100Hz | 16bit | 128kbps"

// ==================== 播放列表 ====================

int64_t termus_playlist_size(void);
const char* termus_playlist_name(int64_t i);  // 显示名(文件名)
const char* termus_playlist_path(int64_t i);  // 完整路径
// 扫描目录中的音频文件 (mp3/flac/wav/ogg/m4a/aac/opus/ape/wv/wma), 返回文件数
int64_t termus_playlist_refresh(const char* dir);

// ==================== 收藏 (桥接层管理) ====================

void termus_fav_toggle(void);              // 切换当前曲目收藏
int64_t termus_fav_count(void);
const char* termus_fav_name(int64_t i);    // 收藏曲目的文件名

// ==================== UI 生命周期 ====================

// Kaula 注册 C ABI 回调函数指针, 然后进入 FTXUI 事件循环 (阻塞至退出)。
// 回调在事件循环线程中被调用, 回调里可以安全地调用上面的控制函数。
typedef void (*termus_cb_play)(int64_t idx);
typedef void (*termus_cb_void)(void);
typedef void (*termus_cb_seek)(double sec);
typedef void (*termus_cb_volume)(int64_t pct);

void termus_ui_start(
    termus_cb_play  cb_play,      // 回车 / 点"播放": 播放选中项
    termus_cb_void  cb_toggle,    // 空格 / 点"播放/暂停"
    termus_cb_void  cb_stop,
    termus_cb_void  cb_next,
    termus_cb_void  cb_prev,
    termus_cb_void  cb_mode,
    termus_cb_seek  cb_seek,      // ←/→: ±5 秒
    termus_cb_volume cb_volume,   // +/-: 音量
    termus_cb_void  cb_speed,
    termus_cb_void  cb_eq,
    termus_cb_void  cb_eq_preset,
    termus_cb_void  cb_refresh,   // 重新扫描目录
    termus_cb_void  cb_fav,       // 收藏切换
    termus_cb_void  cb_exit);     // 退出

// 请求退出事件循环 (Kaula 侧可调用)
void termus_ui_exit(void);

#ifdef __cplusplus
}
#endif
