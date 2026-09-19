#pragma once

#include <windows.h>
#include <functional>
#include <string>

// P1/P2/P3：全屏透明承载窗。
//
// 机制：一个 WS_EX_LAYERED 全屏窗口（alpha 可配，0=全透明、视觉无输入框）内嵌标准
// 单行 Edit 控件（child id = kEditId）。Edit 获得键盘焦点后，系统 IME（水杉/微软拼音）
// 即在其上激活（Edit 是 TSF 集成控件，无需自实现 ITfContextOwner），IME 候选窗跟随
// Edit caret。
//
// P2：Edit 文本变化（EN_CHANGE）经 onTextChanged 回调通知主控刷新"已上屏文本"浮层。
// P3：子类化 Edit 拦截 Enter/Esc 两段式语义——IME 组字中 Enter/Esc 放行给 IME
// （确认候选/取消组合）；非组字态 Enter = 发送请求（带已上屏文本）、Esc = 取消请求。
// P4：失焦自退——WM_ACTIVATE WA_INACTIVE（玩家 Alt-Tab/切走）→ onInactive 回调，
// 主控静默退出打字态（HideQuiet：隐藏但不抢回前台）。
//
// 生命周期：Create 创建并保持隐藏；ShowAndFocus = 进入打字态（清空文本 + 显示 + 抢前台 +
// SetFocus(edit)）；HideAndRestoreFocus = 退出打字态（隐藏 + 前台还给游戏窗口）；
// HideQuiet = 静默退出（仅隐藏，不抢前台）。
class CarrierWindow
{
public:
    ~CarrierWindow();

    // 创建隐藏的承载窗。windowAlpha: LWA_ALPHA 值，0=完全透明。
    bool Create(HINSTANCE hInstance, BYTE windowAlpha);

    // 销毁承载窗及其子控件（析构时自动调用）。
    void Destroy();

    // chat-open：记录 gameHwnd（未来还焦目标），清空上次残留文本，显示承载窗并拿焦。
    void ShowAndFocus(HWND gameHwnd);

    // chat-close：隐藏承载窗，把前台还给 gameHwnd。
    void HideAndRestoreFocus();

    // 静默隐藏（不抢回前台）——用于失焦自退：玩家切走后安静让路。
    void HideQuiet();

    // 绑定"Edit 已上屏文本变化"回调（主控用它刷新迷你浮层；空串=已清空）。
    void SetOnTextChanged(std::function<void(std::wstring)> callback);

    // P3：绑定发送请求（非组字态 Enter，携带已上屏整句）。
    void SetOnSendRequested(std::function<void(std::wstring text)> callback);

    // P3：绑定取消请求（非组字态 Esc）。
    void SetOnCancelRequested(std::function<void()> callback);

    // P4：绑定"承载窗失去激活"回调（玩家切走/Alt-Tab 时触发，主控据此静默退出打字态）。
    void SetOnInactive(std::function<void()> callback);

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static constexpr WORD kEditId = 100; // Edit 控件 id（避开 IDOK=1 的歧义）

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam);

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND gameHwnd_ = nullptr;
    WNDPROC oldEditProc_ = nullptr; // 原 Edit 窗口过程（子类化链）
    std::function<void(std::wstring)> onTextChanged_;
    std::function<void(std::wstring)> onSendRequested_;
    std::function<void()> onCancelRequested_;
    std::function<void()> onInactive_;
    // 失焦回调抑制截止时刻（GetTickCount64）：抢焦/还焦/隐藏期间忽略自身引起的 WA_INACTIVE。
    unsigned long long inactiveSuppressUntilMs_ = 0;
    bool visible_ = false;
};
