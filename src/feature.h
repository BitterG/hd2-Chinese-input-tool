#pragma once

#include "capture.h"

// P4-2：像素特征判定原语（感知层 feature 模块主路径）。
// 纯计算、毫秒级、零第三方依赖。聊天框开关判定由真机定标产物驱动。

namespace feature {

// BGRA32 分量访问（低字节 = Blue）。
inline uint8_t B(uint32_t px) { return static_cast<uint8_t>(px & 0xFF); }
inline uint8_t G(uint32_t px) { return static_cast<uint8_t>((px >> 8) & 0xFF); }
inline uint8_t R(uint32_t px) { return static_cast<uint8_t>((px >> 16) & 0xFF); }

// 两色 RGB 欧氏距离（0..441）。
int ColorDistance(uint32_t a, uint32_t b);

// 采样像素；越界返回 false。
bool SamplePixel(const CapturedFrame &frame, int x, int y, uint32_t &out);

// 区域平均色（RGB 各通道均值拼为 0x00RRGGBB）。失败返回 false。
bool AverageColor(const CapturedFrame &frame, uint32_t &out);

// 帧差统计：两帧须同尺寸。meanDiff=平均 RGB 距离；changedRatio=距离>threshold 的像素占比。
bool FrameDiff(const CapturedFrame &a, const CapturedFrame &b, double &meanDiff,
               double &changedRatio, int threshold = 24);

// ---- HD2 聊天提示面板判别（v2：亮度分层 + 笔画结构） ----
//
// 面板状态：
//   Closed  —— 面板在，显示"开启聊天"文字/图标（未打开）
//   Open    —— 面板在，输入态（气泡图标白亮簇）
//   NoPanel —— 面板不在/不可判（转场/CG/大片亮暗背景），感知不动作
enum class PanelState { Unknown, Closed, Open, NoPanel };

// 面板测量特征（19 张真机样本离线标定，含沙漠/对局/天空/雪地等背景干扰）：
//   runMedRatio  —— 每行"亮度>150 的最长连续亮段"的中位数 / 行宽（0..1）。
//                   文字/图标笔画结构：小（~0.05-0.1）；大片亮背景（天空/雪地）：≈0.7-1.0。
//   pctM150      —— 检测区 150..199 亮度像素占比（"开启聊天"文字笔画主体带）。
//   pctIconW240  —— 图标子区（检测区最左 30% 宽）">240"像素占比。
//                   打开态气泡图标纯白亮簇强特异：Open≈5-6%，其余≈0。
struct PanelFeatures
{
    double runMedRatio = 0.0;
    double pctM150 = 0.0;
    double pctIconW240 = 0.0;
};

// 测量检测区 (x,y,w,h) 的面板特征（越界自动裁剪）。失败返回 false。
bool MeasurePanelFeatures(const CapturedFrame &frame, int x, int y, int w, int h,
                          PanelFeatures &out);

// v2 判别（19 张真机样本 18/19，唯一偏差"雪地天关"判 NoPanel 属保守安全：
// open 由图标特征独立捕获，不受 Closed/NoPanel 误分影响）：
//   1) pctIconW240 > 1.0                 → Open
//   2) runMedRatio < 0.4（结构化亮像素）：
//        pctM150 > 5.0                    → Closed
//        否则                              → NoPanel（暗/无文字）
//   3) 否则（行内大片连续亮段 = 亮背景）   → NoPanel
PanelState ClassifyPanel(const PanelFeatures &features);

// 去抖状态机：原始判定需连续 required 帧稳定为新态才翻转，返回 true 表示状态翻转。
class DebounceState
{
public:
    explicit DebounceState(int required = 3) : required_(required) {}

    void Reset(bool initialState)
    {
        state_ = initialState;
        stableCount_ = 0;
    }

    // 喂一帧原始判定；若状态翻转返回 true，outState 为新状态（否则为当前状态）。
    bool Update(bool raw, bool &outState);

private:
    int required_;
    int stableCount_ = 0;
    bool state_ = false;
};

} // namespace feature
