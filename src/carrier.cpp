#include "carrier.h"

#include "ime.h"

#include <imm.h>
#include <cstdio>
#include <utility>

namespace {

const wchar_t *kClassName = L"Hd2OcrCarrierWnd";

// 把前台窗口设为目标：把本线程输入队列挂到"当前前台线程"与"目标线程"（绕过前台锁定
// 限制），再 SetForegroundWindow，最后解除挂接。游戏为普通权限时有效（对齐参考项目场景）。
void BringToForeground(HWND target)
{
    if (target == nullptr || !IsWindow(target))
    {
        return;
    }
    const HWND fg = GetForegroundWindow();
    if (fg == target)
    {
        return; // 已是前台
    }
    const DWORD current = GetCurrentThreadId();
    const DWORD fgThread = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
    bool attachedFg = false;
    if (fgThread != 0 && fgThread != current)
    {
        attachedFg = AttachThreadInput(current, fgThread, TRUE) != FALSE;
    }
    bool attachedTarget = false;
    if (targetThread != 0 && targetThread != current && targetThread != fgThread)
    {
        attachedTarget = AttachThreadInput(current, targetThread, TRUE) != FALSE;
    }
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (attachedTarget)
    {
        AttachThreadInput(current, targetThread, FALSE);
    }
    if (attachedFg)
    {
        AttachThreadInput(current, fgThread, FALSE);
    }
}

// IME 是否正处于组字态（有未确认的组合串）。TSF 输入法经 IMM32 兼容层同样可查。
bool IsImeComposing(HWND edit)
{
    HIMC himc = ImmGetContext(edit);
    if (himc == nullptr)
    {
        return false;
    }
    const LONG len = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(edit, himc);
    return len > 0;
}

// 抢焦/还焦/隐藏会引发承载窗自身的 WA_INACTIVE，那不是"玩家切走"，需抑制回调。
constexpr unsigned long long kInactiveSuppressMs = 600;

} // namespace

CarrierWindow::~CarrierWindow()
{
    Destroy();
}

void CarrierWindow::Destroy()
{
    if (hwnd_ != nullptr)
    {
        // 子 Edit 随父窗口一并销毁。
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    edit_ = nullptr;
    oldEditProc_ = nullptr;
    visible_ = false;
}

void CarrierWindow::SetOnTextChanged(std::function<void(std::wstring)> callback)
{
    onTextChanged_ = std::move(callback);
}

void CarrierWindow::SetOnSendRequested(std::function<void(std::wstring)> callback)
{
    onSendRequested_ = std::move(callback);
}

void CarrierWindow::SetOnCancelRequested(std::function<void()> callback)
{
    onCancelRequested_ = std::move(callback);
}

void CarrierWindow::SetOnInactive(std::function<void()> callback)
{
    onInactive_ = std::move(callback);
}

bool CarrierWindow::Create(HINSTANCE hInstance, BYTE windowAlpha)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kClassName;
    if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        printf("[carrier] RegisterClass failed lastError=%lu\n", GetLastError());
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"",
                            WS_POPUP, 0, 0, screenW, screenH, nullptr, nullptr, hInstance, this);
    if (hwnd_ == nullptr)
    {
        printf("[carrier] CreateWindowEx failed lastError=%lu\n", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, windowAlpha, LWA_ALPHA);

    edit_ = CreateWindowExW(0, L"Edit", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                            0, 0, screenW, screenH, hwnd_, reinterpret_cast<HMENU>(kEditId),
                            hInstance, nullptr);
    if (edit_ == nullptr)
    {
        printf("[carrier] Edit create failed lastError=%lu\n", GetLastError());
        return false;
    }
    // 子类化 Edit：拦截非组字态 Enter（发送）/ Esc（取消）。self 存于 Edit 的 GWLP_USERDATA。
    SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    oldEditProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc)));

    // 初始隐藏，仅打字态激活（非打字态不挡鼠标/不抢输入）。
    ShowWindow(hwnd_, SW_HIDE);
    printf("[carrier] created hwnd=%p edit=%p alpha=%u (%dx%d, hidden)\n", hwnd_, edit_,
           windowAlpha, screenW, screenH);
    fflush(stdout);
    return true;
}

void CarrierWindow::ShowAndFocus(HWND gameHwnd)
{
    gameHwnd_ = gameHwnd;
    if (hwnd_ == nullptr)
    {
        return;
    }
    // 抢焦会引起承载窗自身的激活变化（WA_ACTIVE/WA_INACTIVE 抖动）→ 抑制失焦回调。
    inactiveSuppressUntilMs_ = GetTickCount64() + kInactiveSuppressMs;
    // 进入打字态：清空上次残留文本，并通知浮层清屏。
    if (edit_ != nullptr)
    {
        SetWindowTextW(edit_, L"");
    }
    if (onTextChanged_)
    {
        onTextChanged_(L"");
    }
    ShowWindow(hwnd_, SW_SHOW);
    BringToForeground(hwnd_);
    SetFocus(edit_);
    visible_ = true;
    // 呼出输入框 → 切一次中文输入法（承载窗与主控同线程，ActivateKeyboardLayout 直接生效）。
    SwitchToChineseInput(hwnd_);
    const HWND fg = GetForegroundWindow();
    printf("[carrier] shown  hwnd=%p foreground-now=%p focus-now=%p fg-is-carrier=%d\n", hwnd_,
           fg, GetFocus(), fg == hwnd_ ? 1 : 0);
    fflush(stdout);
}

void CarrierWindow::HideAndRestoreFocus()
{
    if (hwnd_ != nullptr && visible_)
    {
        // 还焦会引起失活抖动 → 抑制失焦回调（否则会被自己触发的 WA_INACTIVE 误退出）。
        inactiveSuppressUntilMs_ = GetTickCount64() + kInactiveSuppressMs;
        // 先还焦（此刻本进程仍是前台进程，SetForegroundWindow 更易成功），再隐藏承载窗。
        BringToForeground(gameHwnd_);
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        printf("[carrier] hidden, restoring foreground to game=%p (fg-now=%p)\n", gameHwnd_,
               GetForegroundWindow());
        fflush(stdout);
        if (GetForegroundWindow() != gameHwnd_)
        {
            BringToForeground(gameHwnd_); // 重试一次
        }
        // 退出输入 → 切回英文输入法一次（对游戏窗口尽力：引擎不处理该消息则无效）。
        SwitchToEnglishInput(gameHwnd_);
    }
}

void CarrierWindow::HideQuiet()
{
    if (hwnd_ != nullptr && visible_)
    {
        inactiveSuppressUntilMs_ = GetTickCount64() + kInactiveSuppressMs;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        printf("[carrier] hidden quietly (inactive, no foreground steal)\n");
        fflush(stdout);
        // 注：这是"玩家切走"路径，不改动输入法（避免干扰其它窗口）。
    }
}

LRESULT CALLBACK CarrierWindow::EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam)
{
    auto *self = reinterpret_cast<CarrierWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 吞掉回车的 WM_CHAR（\r/\n）：单行 Edit 无默认按钮时，默认处理会响系统提示音（beep）。
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\n'))
    {
        return 0;
    }
    if (message == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
        {
            if (self != nullptr && IsImeComposing(hwnd))
            {
                // IME 组字中：Enter=确认候选、Esc=取消组合——放行给 IME 处理。
                return CallWindowProcW(self->oldEditProc_ != nullptr ? self->oldEditProc_
                                                                     : DefWindowProcW,
                                       hwnd, message, wParam, lParam);
            }
            if (self != nullptr)
            {
                if (wParam == VK_RETURN)
                {
                    // 非组字态 Enter → 发送请求（携带已上屏整句）；空文本等同放弃。
                    const int len = GetWindowTextLengthW(hwnd);
                    if (len > 0)
                    {
                        std::wstring text;
                        text.resize(static_cast<size_t>(len) + 1);
                        GetWindowTextW(hwnd, text.data(), len + 1);
                        text.resize(static_cast<size_t>(len));
                        if (self->onSendRequested_)
                        {
                            self->onSendRequested_(std::move(text));
                        }
                    }
                    else if (self->onCancelRequested_)
                    {
                        self->onCancelRequested_();
                    }
                }
                else if (wParam == VK_ESCAPE)
                {
                    // 非组字态 Esc → 取消退出打字态。
                    if (self->onCancelRequested_)
                    {
                        self->onCancelRequested_();
                    }
                }
            }
            return 0; // 吞掉按键，不让 Edit 产生其它默认行为
        }
    }
    if (self != nullptr && self->oldEditProc_ != nullptr)
    {
        return CallWindowProcW(self->oldEditProc_, hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK CarrierWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *self = reinterpret_cast<CarrierWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
    case WM_CREATE:
    {
        const auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_ACTIVATE:
        // 玩家切走（Alt-Tab/点击其它窗口）→ 静默退出打字态。
        // 注意：抢焦/还焦/隐藏承载窗自身也会引发 WA_INACTIVE，那不是玩家切走——
        // 在抑制窗口期内忽略，避免"刚呼出就被自己判为失焦而退出"的时序竞争。
        if (self != nullptr && LOWORD(wParam) == WA_INACTIVE && self->onInactive_ &&
            GetTickCount64() >= self->inactiveSuppressUntilMs_)
        {
            self->onInactive_();
        }
        break;
    case WM_SIZE:
        if (self != nullptr && self->edit_ != nullptr)
        {
            MoveWindow(self->edit_, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (self != nullptr && LOWORD(wParam) == kEditId)
        {
            const WORD code = HIWORD(wParam);
            if (code == EN_CHANGE)
            {
                // 文本变化（含 IME 上屏）→ 读最新文本通知主控刷新浮层。
                const int len = GetWindowTextLengthW(self->edit_);
                if (len > 0)
                {
                    std::wstring text;
                    text.resize(static_cast<size_t>(len) + 1);
                    GetWindowTextW(self->edit_, text.data(), len + 1);
                    text.resize(static_cast<size_t>(len));
                    if (self->onTextChanged_)
                    {
                        self->onTextChanged_(std::move(text));
                    }
                }
                else if (self->onTextChanged_)
                {
                    self->onTextChanged_(L"");
                }
            }
            return 0; // 其余 Edit 通知一律忽略（Enter 语义已由子类拦截）
        }
        break;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
