// cover.h — 专辑封面: FFmpeg 提取内嵌封面 + half-block 像素渲染 (fastfetch/chafa 同款)
// 两步接口:
//   1. coverDecode(path) — 提取内嵌封面并解码为原始 RGB(重操作, 切歌时调用一次)
//   2. coverCellsFromRgb(rgb, w, h, cols, rows) — 按目标字符格尺寸动态缩放+抖动+生成色块
//      (轻操作, 每次渲染按当前终端尺寸调用, 支持全屏/缩放自适应清晰度)
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

// 一个 half-block 像素格: 上半/下半两个像素的 RGB
struct CoverCell {
    uint8_t r1, g1, b1;  // 上半像素色(前景)
    uint8_t r2, g2, b2;  // 下半像素色(背景)
};

// 提取内嵌封面并解码为原始 RGB。无封面/失败返回 invalid。
CoverImage coverDecode(const std::string& audioPath);

// 按目标字符格尺寸生成 half-block 色块网格(行主序)。
// 每个字符格 = 源图 1x2 像素; 内部做双线性缩放 + Floyd-Steinberg 抖动。
// cols x rows 字符格 = cols x (rows*2) 像素。失败返回空。
std::vector<CoverCell> coverCellsFromRgb(const CoverImage& img,
                                         int targetCols, int targetRows);
