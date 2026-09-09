#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

// P4-1：区域截屏（感知层 capture 模块）。
// 只抓校准过的目标区域（聊天框区域），不做整屏/逐帧——满足 DESIGN.md 性能准则。

struct CapturedFrame
{
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // BGRA32（低字节=B），行主序，size = width*height
    bool empty() const { return pixels.empty(); }
};

// 抓取屏幕指定区域（物理像素坐标）。失败返回 false（GetLastError 可查）。
bool CaptureScreenRegion(int x, int y, int w, int h, CapturedFrame &out);
