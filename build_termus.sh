#!/usr/bin/env bash
# build_termus.sh — 构建 termus 桥接层静态库 libtermus_bridge.a
#
# 前置依赖(本地路径, 按需修改):
#   FTXUI_ROOT    FTXUI 源码树 (C:/Code/ftxui)
#   FFMPEG_ROOT   FFmpeg 裁剪源码树 (C:/Code/third_party/FFmpeg-n7.1)
#   CLANGXX       llvm-mingw 的 clang++ (C:/Program Files/llvm-mingw/bin)
#
# 产出: libtermus_bridge.a (与 termus.kl 同目录)
# 然后编译 Kaula 主程序 (kaula.json 已配置好, 直接):
#   kaulac --release termus.kl
set -e

FTXUI_ROOT="${FTXUI_ROOT:-C:/Code/ftxui}"
FFMPEG_ROOT="${FFMPEG_ROOT:-C:/Code/third_party/FFmpeg-n7.1}"
LLVM_MINGW="${LLVM_MINGW:-C:/Program Files/llvm-mingw}"
CLANGXX="${CLANGXX:-$LLVM_MINGW/bin/clang++.exe}"
AR="${AR:-$LLVM_MINGW/bin/llvm-ar.exe}"

HERE="$(cd "$(dirname "$0")" && pwd)"
# Windows 原生程序(clang++/llvm-ar)需要 Windows 风格路径
if command -v cygpath >/dev/null 2>&1; then
  HERE_W="$(cygpath -w "$HERE")"
  FTXUI_ROOT_W="$(cygpath -w "$FTXUI_ROOT")"
  FFMPEG_ROOT_W="$(cygpath -w "$FFMPEG_ROOT")"
else
  HERE_W="$HERE"; FTXUI_ROOT_W="$FTXUI_ROOT"; FFMPEG_ROOT_W="$FFMPEG_ROOT"
fi
BUILD="$HERE/build"
mkdir -p "$BUILD"

# -fno-exceptions: 缺省会与 libc++ 的 length_error weak 符号冲突导致 lld 链接失败
FLAGS="-std=c++23 -O2 -DUNICODE -D_UNICODE -fwrapv -fno-strict-aliasing -fno-exceptions -DFTXUI_MICROSOFT_TERMINAL_FALLBACK"
INCS="-I$HERE_W -I$FTXUI_ROOT_W/include -I$FTXUI_ROOT_W/src -I$FFMPEG_ROOT_W"

OBJS=()

compile() {  # compile <src> <obj>
  local src="$1" obj="$2"
  if command -v cygpath >/dev/null 2>&1; then
    src="$(cygpath -w "$src")"
    obj="$(cygpath -w "$obj")"
  fi
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[build] $src"
    "$CLANGXX" $FLAGS $INCS -c "$src" -o "$obj"
  fi
  OBJS+=("$obj")
}

# 桥接层 + 播放核心
compile "$HERE/termus_bridge.cpp" "$BUILD/termus_bridge.o"
compile "$HERE/src/player.cpp"    "$BUILD/player.o"
compile "$HERE/src/audio_out.cpp" "$BUILD/audio_out.o"
compile "$HERE/src/cover.cpp"     "$BUILD/cover.o"

# FTXUI 源码(排除测试/fuzzer)
# 注意: dom/color.cpp 与 screen/color.cpp 等存在同名源文件,
# 必须用相对路径生成唯一 obj 名, 否则互相覆盖导致链接缺符号
count=0
while IFS= read -r -d '' f; do
  rel="${f#"$FTXUI_ROOT"/src/}"
  safe="$(echo "$rel" | tr '/' '_')"
  compile "$f" "$BUILD/ftxui_${safe%.cpp}.o"
  count=$((count + 1))
done < <(find "$FTXUI_ROOT/src" -name '*.cpp' -print0 \
         | grep -zv '_test\.cpp$\|_fuzzer\.cpp$\|benchmark_test\.cpp$' || true)
echo "[build] FTXUI sources: $count"

# 归档
LIB_A="$HERE/libtermus_bridge.a"
if command -v cygpath >/dev/null 2>&1; then
  LIB_A_W="$(cygpath -w "$LIB_A")"
else
  LIB_A_W="$LIB_A"
fi
if [ -f "$LIB_A" ]; then rm "$LIB_A"; fi
"$AR" rcs "$LIB_A_W" "${OBJS[@]}"
echo "[build] OK -> $LIB_A (${#OBJS[@]} objects)"
