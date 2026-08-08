# Termus — Kaula 版终端音乐播放器

`termus` = **TERM**inal + **MUS**ic。用 Kaula 编写的终端音乐播放器示例，
演示 Kaula 通过 C 桥接层调用三块底层：

| 底层 | 语言 | 用途 |
|------|------|------|
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | C++ | 播放列表菜单 / 按钮 / 滑块 / 布局 |
| [FFmpeg](https://ffmpeg.org) | C | 音频解码 + atempo 变速滤镜 |
| waveOut (winmm) | Windows API | 音频输出 |

## 架构

```
termus.kl (Kaula: 全部应用逻辑)
   │  extern fn 调用 / C ABI 回调注册
   ▼
termus_bridge.h/.cpp (C++ 桥接层, 编译为 libtermus_bridge.a)
   │
   ├── FTXUI 组件 + ScreenInteractive 事件循环 (UI 线程)
   ├── Player 核心: FFmpeg 解码线程 + 环形缓冲 (src/player.cpp)
   └── AudioOut: waveOut 输出线程 (src/audio_out.cpp)
```

- **Kaula 侧**（termus.kl）：`extern fn` 声明桥接层 API，`type` 别名声明回调
  函数指针类型（`type CBPlay = void(int)void`），Kaula 函数直接作为 C ABI
  回调注册给 FTXUI 按钮/快捷键。事件逻辑（播放/暂停/切歌/变速/EQ/收藏）全部
  由 Kaula 代码处理。
- **桥接层**（termus_bridge.cpp）：把 FTXUI 的 C++ 类 API 和 Player 播放核心
  封装成纯 C 函数，Kaula 生成的 C 代码可以直接链接。

## 功能

- 播放列表：自动识别 ~/Music（getenv USERPROFILE 运行时读取），目录不存在/为空
  时回退 C:/Windows/Media；支持 mp3/flac/wav/ogg/m4a/aac/opus/ape/wv/wma
- 控制：播放/暂停/上一首/下一首/停止（按钮 + 快捷键）
- 播放模式循环：顺序 → 列表循环 → 单曲循环 → 随机
- 变速：0.75x → 1x → 1.25x → 1.5x → 2x → 3x（FFmpeg atempo 保调）
- EQ：开/关 + 8 个预设（Flat/Classical/Dance/Pop/Rock/Jazz/Bass+/Treble+）
- 音量滑块 + +/- 快捷键，±5 秒快进快退
- 收藏列表（♥/♡）
- 状态栏显示：播放状态/曲目进度/编码信息/音频设备状态

快捷键：`回车`=播放选中  `空格`=播放/暂停  `←/→`=±5秒  `+/-`=音量  `M`=模式  `R`=刷新  `Q`=退出

## 构建

前置依赖（本机路径，可在 build_termus.sh / kaula.json 中修改）：

- Kaula 编译器：`C:/Code/kaula/build/bin`（kaulac）
- Kaula 标准库：`C:/Code/kaula/std`
- FTXUI 源码树：`C:/Code/ftxui`
- FFmpeg 裁剪源码树（含静态库）：`C:/Code/third_party/FFmpeg-n7.1`
- llvm-mingw（clang++/llvm-ar 在 PATH 或 `C:/Program Files/llvm-mingw/bin`）

```bash
# 1. 编译桥接层静态库 (libtermus_bridge.a)
bash build_termus.sh

# 2. 编译 Kaula 主程序 (termus.exe) — kaula.json 已含全部链接配置
kaulac --release termus.kl

# 3. 运行
./termus.exe
```

## 目录

```
termus/
├── termus.kl          # Kaula 主程序（事件回调 + 控制逻辑）
├── termus_bridge.h    # C 桥接层 API（Kaula extern fn 的对应声明）
├── termus_bridge.cpp  # C++ 桥接层实现（FTXUI UI + Player 封装）
├── build_termus.sh    # 桥接层静态库构建脚本
├── kaula.json         # kaulac 编译配置（c_flags/c_libs）
└── src/               # 播放核心（FFmpeg 解码 + waveOut 输出）
    ├── player.h/.cpp
    └── audio_out.h/.cpp
```

## Kaula FFI 要点（本示例踩过的坑）

1. **extern 参数位置的函数指针不吃尾部返回类型**：`cb: void(int)void` 直接写
   在 extern 参数里会解析失败，必须用 `type` 别名：
   `type CBPlay = void(int)void` 然后 `cb_play: CBPlay`。
2. **字符串字面量是 `String` 结构体**，不能直接传 `char*` 参数。用
   `string s = string_create("...")` 再传 `s.ptr`。
3. **Kaula 函数默认生成标准 C ABI**，可直接作为 C 函数指针注册回调
   （本例 FTXUI 按钮/快捷键回调全部由 Kaula 函数处理）。
4. 编译期 `type` 别名的 C typedef 现在生成在 extern 声明之前
   （编译器修复，见 kaula codegen.go No-main 路径）。

## 参考

- C++ 完整版（含 LRC 歌词/EQ 可视化/配置持久化）：`C:/Code/ter-music/termus`
- 原版 Linux C 项目：`C:/Code/ter-music`
- Kaula FFI 文档：`C:/Code/kaula/docs/tutorials/extern-and-ffi.md`
