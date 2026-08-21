// cover.h — 专辑封面: FFmpeg 提取内嵌封面 + quadrant(2x2)像素渲染
// 两步接口:
//   1. coverDecode(path) — 提取内嵌封面并解码为原始 RGB(重操作, 切歌时调用一次)
//   2. coverCellsFromRgb(rgb, w, h, cols, rows) — 按目标字符格尺寸动态缩放+生成色块
//      (轻操作, 每次渲染按当前终端尺寸调用, 支持全屏/缩放自适应清晰度)
// 渲染: 每字符格 = 2x2 源像素(quadrant 块字符), 形状分辨率横向翻倍;
//       fg = 实心象限平均色, bg = 空心象限平均色(每格 2 色, 终端 SGR 极限)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 解码结果: 原始 RGB 像素 + 宽高
struct CoverImage {
    std::vector<uint8_t> rgb;  // w*h*3, RGB888
    int w = 0, h = 0;
    bool valid() const { return !rgb.empty() && w > 0 && h > 0; }
};

// 一个 quadrant 像素格: 2x2 象限, 实心象限=前景色, 空心象限=背景色
struct CoverCell {
    std::string ch;   // quadrant 块字符 (UTF-8, U+2580-259F 或空格)
    uint8_t r1, g1, b1;  // 前景色(实心象限平均)
    uint8_t r2, g2, b2;  // 背景色(空心象限平均)
};

// 提取内嵌封面并解码为原始 RGB。无封面/失败返回 invalid。
CoverImage coverDecode(const std::string& audioPath);

// 按目标字符格尺寸生成 quadrant 色块网格(行主序)。
// 每个字符格 = 源图 2x2 像素(内部 Lanczos 缩放 + 最优 2+2 象限分配)。
// cols x rows 字符格 = cols*2 x rows*2 像素。失败返回空。
std::vector<CoverCell> coverCellsFromRgb(const CoverImage& img,
                                         int targetCols, int targetRows);
