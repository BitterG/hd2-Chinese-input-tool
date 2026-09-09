#include "feature.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace feature {

int ColorDistance(uint32_t a, uint32_t b)
{
    const double dr = static_cast<double>(static_cast<int>(R(a)) - static_cast<int>(R(b)));
    const double dg = static_cast<double>(static_cast<int>(G(a)) - static_cast<int>(G(b)));
    const double db = static_cast<double>(static_cast<int>(B(a)) - static_cast<int>(B(b)));
    return static_cast<int>(std::sqrt(dr * dr + dg * dg + db * db));
}

bool SamplePixel(const CapturedFrame &frame, int x, int y, uint32_t &out)
{
    if (frame.empty() || x < 0 || y < 0 || x >= frame.width || y >= frame.height)
    {
        return false;
    }
    out = frame.pixels[static_cast<size_t>(y) * static_cast<size_t>(frame.width) +
                       static_cast<size_t>(x)];
    return true;
}

bool AverageColor(const CapturedFrame &frame, uint32_t &out)
{
    if (frame.empty())
    {
        return false;
    }
    unsigned long long sumB = 0;
    unsigned long long sumG = 0;
    unsigned long long sumR = 0;
    for (uint32_t pixel : frame.pixels)
    {
        sumB += B(pixel);
        sumG += G(pixel);
        sumR += R(pixel);
    }
    const size_t count = frame.pixels.size();
    out = (static_cast<uint32_t>(sumR / count) << 16) |
          (static_cast<uint32_t>(sumG / count) << 8) | static_cast<uint32_t>(sumB / count);
    return true;
}

bool FrameDiff(const CapturedFrame &a, const CapturedFrame &b, double &meanDiff,
               double &changedRatio, int threshold)
{
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
    {
        return false;
    }
    const size_t count = a.pixels.size();
    const long long thresholdSq = static_cast<long long>(threshold) * threshold;
    double sumDistance = 0.0;
    size_t changed = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const long long dr = static_cast<long long>(R(a.pixels[i])) - R(b.pixels[i]);
        const long long dg = static_cast<long long>(G(a.pixels[i])) - G(b.pixels[i]);
        const long long db = static_cast<long long>(B(a.pixels[i])) - B(b.pixels[i]);
        const long long distSq = dr * dr + dg * dg + db * db;
        sumDistance += std::sqrt(static_cast<double>(distSq));
        if (distSq > thresholdSq)
        {
            ++changed;
        }
    }
    meanDiff = sumDistance / static_cast<double>(count);
    changedRatio = static_cast<double>(changed) / static_cast<double>(count);
    return true;
}

bool MeasurePanelFeatures(const CapturedFrame &frame, int x, int y, int w, int h,
                          PanelFeatures &out)
{
    out = PanelFeatures{};
    if (frame.empty() || w <= 0 || h <= 0)
    {
        return false;
    }
    // 手动 clamp（避免 windows.h 的 min/max 宏污染）。
    int xStart = x;
    if (xStart < 0)
    {
        xStart = 0;
    }
    int yStart = y;
    if (yStart < 0)
    {
        yStart = 0;
    }
    int xEnd = x + w;
    if (xEnd > frame.width)
    {
        xEnd = frame.width;
    }
    int yEnd = y + h;
    if (yEnd > frame.height)
    {
        yEnd = frame.height;
    }
    if (xStart >= xEnd || yStart >= yEnd)
    {
        return false;
    }
    const int rowW = xEnd - xStart;

    // 每行"亮度>150 最长连续亮段"收集；同时统计 M150(150..199) 占比。
    std::vector<int> runs;
    runs.reserve(static_cast<size_t>(yEnd - yStart));
    size_t total = 0;
    size_t m150 = 0;
    for (int yy = yStart; yy < yEnd; ++yy)
    {
        int cur = 0;
        int best = 0;
        for (int xx = xStart; xx < xEnd; ++xx)
        {
            const uint32_t pixel =
                frame.pixels[static_cast<size_t>(yy) * static_cast<size_t>(frame.width) +
                             static_cast<size_t>(xx)];
            const int lum = (static_cast<int>(R(pixel)) + G(pixel) + B(pixel)) / 3;
            ++total;
            if (lum > 240)
            {
                // 近纯白：Open 气泡亮簇或极亮背景——不进入 M150 带
            }
            else if (lum >= 150)
            {
                ++m150; // 150..239 均计入 M150 带（含 200-239 亮灰背景，供 Closed 判断）
            }
            if (lum > 150)
            {
                ++cur;
                if (cur > best)
                {
                    best = cur;
                }
            }
            else
            {
                cur = 0;
            }
        }
        runs.push_back(best);
    }
    if (total == 0)
    {
        return false;
    }
    std::sort(runs.begin(), runs.end());
    out.runMedRatio = static_cast<double>(runs[runs.size() / 2]) / static_cast<double>(rowW);
    out.pctM150 = 100.0 * static_cast<double>(m150) / static_cast<double>(total);

    // 图标子区：检测区最左 30% 宽内 ">240" 像素占比（气泡图标白亮簇位置）。
    int iconW = rowW * 3 / 10;
    if (iconW < 1)
    {
        iconW = 1;
    }
    size_t iconTotal = 0;
    size_t iconW240 = 0;
    for (int yy = yStart; yy < yEnd; ++yy)
    {
        for (int xx = xStart; xx < xStart + iconW && xx < xEnd; ++xx)
        {
            const uint32_t pixel =
                frame.pixels[static_cast<size_t>(yy) * static_cast<size_t>(frame.width) +
                             static_cast<size_t>(xx)];
            const int lum = (static_cast<int>(R(pixel)) + G(pixel) + B(pixel)) / 3;
            ++iconTotal;
            if (lum > 240)
            {
                ++iconW240;
            }
        }
    }
    if (iconTotal > 0)
    {
        out.pctIconW240 = 100.0 * static_cast<double>(iconW240) / static_cast<double>(iconTotal);
    }
    return true;
}

PanelState ClassifyPanel(const PanelFeatures &features)
{
    if (features.pctIconW240 > 1.0)
    {
        return PanelState::Open;
    }
    if (features.runMedRatio < 0.4)
    {
        if (features.pctM150 > 5.0)
        {
            return PanelState::Closed;
        }
        return PanelState::NoPanel; // 暗/无文字内容
    }
    return PanelState::NoPanel; // 行内大片连续亮段 = 亮背景主导
}

bool DebounceState::Update(bool raw, bool &outState)
{
    if (raw == state_)
    {
        stableCount_ = 0; // 回到一致，重置计数
        outState = state_;
        return false;
    }
    ++stableCount_;
    if (stableCount_ >= required_)
    {
        state_ = raw;
        stableCount_ = 0;
        outState = state_;
        return true;
    }
    outState = state_;
    return false;
}

} // namespace feature
