#include "carrier.h"

#include <cstdio>

namespace {

const wchar_t *kClassName = L"Hd2OcrCarrierWnd";

// 把前台窗口设为目标：先把本线程输入队列挂到"当前前台线程"（绕过前台锁定限制），
// 再 SetForegroundWindow，最后解除挂接。游戏/目标为普通权限时有效（对齐参考项目场景）。
void BringToForeground(HWND target)
{
    if (target == nullptr || !IsWindow(target))
    {
        return;
    }
    const DWORD current = GetCurrentThreadId();
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = false;
    if (fgThread != 0 && fgThread != current)
    {
        attached = AttachThreadInput(current, fgThread, TRUE) != FALSE;
    }
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (attached)
    {
        AttachThreadInput(current, fgThread, FALSE);
    }
}

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
    visible_ = false;
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
                            0, 0, screenW, screenH, hwnd_, nullptr, hInstance, nullptr);
    if (edit_ == nullptr)
    {
        printf("[carrier] Edit create failed lastError=%lu\n", GetLastError());
        return false;
    }
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
    ShowWindow(hwnd_, SW_SHOW);
    BringToForeground(hwnd_);
    SetFocus(edit_);
    visible_ = true;
    const HWND fg = GetForegroundWindow();
    printf("[carrier] shown  hwnd=%p foreground-now=%p focus-now=%p fg-is-carrier=%d\n", hwnd_,
           fg, GetFocus(), fg == hwnd_ ? 1 : 0);
    fflush(stdout);
}

void CarrierWindow::HideAndRestoreFocus()
{
    if (hwnd_ != nullptr && visible_)
    {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        printf("[carrier] hidden, restoring foreground to game=%p\n", gameHwnd_);
        fflush(stdout);
        BringToForeground(gameHwnd_);
    }
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
    case WM_SIZE:
        if (self != nullptr && self->edit_ != nullptr)
        {
            MoveWindow(self->edit_, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            return 0; // 单行 Edit 回车 → IDOK；忽略（Enter 两段式语义 P3 处理）
        }
        break;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
