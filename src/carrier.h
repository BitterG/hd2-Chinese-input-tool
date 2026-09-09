#pragma once

#include <windows.h>

// P1：全屏透明承载窗。
//
// 机制：一个 WS_EX_LAYERED 全屏窗口（alpha 可配，0=全透明、视觉无输入框）内嵌标准
// 单行 Edit 控件。Edit 获得键盘焦点后，系统 IME（水杉/微软拼音）即在其上激活
// （Edit 是 TSF 集成控件，无需自实现 ITfContextOwner），IME 候选窗跟随 Edit caret。
//
// 生命周期：Create 创建并保持隐藏；ShowAndFocus = 进入打字态（显示 + 抢前台 +
// SetFocus(edit)）；HideAndRestoreFocus = 退出打字态（隐藏 + 前台还给记录的游戏窗口）。
class CarrierWindow
{
public:
    ~CarrierWindow();

    // 创建隐藏的承载窗。windowAlpha: LWA_ALPHA 值，0=完全透明。
    bool Create(HINSTANCE hInstance, BYTE windowAlpha);

    // 销毁承载窗及其子控件（析构时自动调用）。
    void Destroy();

    // chat-open：记录 gameHwnd（未来还焦目标），显示承载窗并把键盘焦点给 Edit。
    void ShowAndFocus(HWND gameHwnd);

    // chat-close：隐藏承载窗，把前台还给 gameHwnd。
    void HideAndRestoreFocus();

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND gameHwnd_ = nullptr;
    bool visible_ = false;
};
