#pragma once

#include <functional>
#include <windows.h>

// 感知层唯一对外接口 —— 骨架阶段与感知阶段共用（见 ../SENSING.md §3）。
// onEvent(chatOpen): true=进入打字态（聊天框开）, false=退出打字态（聊天框关）。
struct ISensingSource
{
    virtual void Start(std::function<void(bool chatOpen)> onEvent) = 0;
    virtual void Stop() = 0;

    // 应用内自主退出（发送/Esc）后同步内部状态（默认空实现）。
    // 热键源：复位 toggle 状态，防下一次 F8 需按两次；
    // 像素源：施加抑制期，防残留聊天框特征立即回环重进。
    virtual void ResetToClosed() {}

    virtual ~ISensingSource() = default;
};

// 骨架占位实现（P0）：RegisterHotKey(F8) 手动模拟"聊天框开/关"事件。
// 感知阶段由 PixelSensingSource 等替换，主控不感知差异。
class HotkeySensingSource final : public ISensingSource
{
public:
    void Start(std::function<void(bool chatOpen)> onEvent) override;
    void Stop() override;

    // 消息循环收到 WM_HOTKEY 时调用；返回是否被本源消费。
    bool HandleHotkey(WPARAM hotkeyId);

    void ResetToClosed() override;

    bool running() const { return running_; }

private:
    static constexpr int kHotkeyId = 1;
    static constexpr UINT kToggleVk = VK_F8; // 单键 toggle，无修饰键（沿用探针"单键最可靠"约定）

    std::function<void(bool chatOpen)> onEvent_;
    bool chatOpen_ = false;
    bool running_ = false;
};
