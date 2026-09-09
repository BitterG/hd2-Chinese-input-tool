#pragma once

#include "sensing.h"

#include <atomic>
#include <thread>

// P4-2：像素特征感知源 —— 自动检测 HD2 右下聊天提示面板开/关。
//
// 设计（对应 SENSING.md 与真机四态定标）：
//  - 前台须为 HD2 游戏才采样（打字态前台=承载窗等非游戏时暂停，不产生事件）；
//  - ROI 按客户区比例定位（跨分辨率换算，RoiSpec）；判别子段取面板左侧约 62% 宽
//    （图标+文字区，定标区域）；
//  - 每帧 capture → 亮度分层（BandStats）→ 四态判定（ClassifyPanelState）；
//  - Closed/Open 经去抖（连续 debounceFrames 帧一致）翻转才发 ChatOpen/ChatClose 事件；
//    NoPanel（转场/CG）不动作，防止误触发；
//  - ResetToClosed()：应用内退出后施加抑制期，防残留聊天框特征立即回环重进。
//
// 注意：事件回调在轮询线程执行；当前用于日志观察（pixeldemo），接入 UI/承载窗时
// 需投递回主线程。
class PixelSensingSource final : public ISensingSource
{
public:
    // ROI：相对 HD2 客户区的比例（0..1）。默认值由 2560x1440 真机样本标定。
    struct RoiSpec
    {
        double relX = 0.793; // 客户区左缘起点
        double relY = 0.886; // 客户区上缘起点
        double relW = 0.188; // 面板宽（图标+文字+输入区）
        double relH = 0.0375;
    };

    // 判别子段占面板宽的比例（左侧图标+文字区）。
    static constexpr double kDetectWidthRatio = 0.62;
    // ResetToClosed 的默认抑制时长（参数 0 时使用；Esc/手动退出防自动重进）。
    static constexpr unsigned long long kSuppressOpenMs = 1500;

    explicit PixelSensingSource(RoiSpec roi = RoiSpec{}, int intervalMs = 250,
                                int debounceFrames = 3);

    void Start(std::function<void(bool chatOpen)> onEvent) override;
    void Stop() override;
    // suppressMs=0 用默认 kSuppressOpenMs；否则用给定时长（发送成功场景传短值以便连发）。
    void ResetToClosed(unsigned long long suppressMs = 0) override;

    bool running() const { return running_.load(); }

private:
    void Loop();

    RoiSpec roi_;
    int intervalMs_;
    int debounceFrames_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::function<void(bool chatOpen)> onEvent_;
    std::atomic<unsigned long long> suppressOpenUntilMs_{0};
};
