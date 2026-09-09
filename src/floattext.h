#pragma once

#include <windows.h>
#include <string>

// P2：打字态"已上屏文本"迷你浮层 —— 解决盲打。
//
// 机制：无焦点 TopMost layered 窗口（WS_EX_NOACTIVATE，不抢键盘焦点），黑底白字自绘，
// 屏幕底部中央固定位置（后续可配置/跟随游戏聊天框）。仅打字态显示（随承载窗显隐）。
// 文本由主控从承载窗 Edit 的 EN_CHANGE 回调喂入——IME 组合中的拼音不进入 Edit 文本，
// 浮层天然只显示"已上屏汉字"，与"无输入框 + 防盲打"设计一致。
class TextOverlay
{
public:
    ~TextOverlay();

    // 创建浮层（初始隐藏）。失败返回 false 并打印原因。
    bool Create(HINSTANCE hInstance);

    void Show(); // SW_SHOWNOACTIVATE：显示但不抢焦点
    void Hide();

    // 更新显示的文本（空文本即无内容）。同线程调用，立即重绘。
    void SetText(const std::wstring &text);

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::wstring text_;
    bool visible_ = false;
};
