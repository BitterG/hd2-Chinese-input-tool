#include "floattext.h"

#include <cstdio>

namespace {

const wchar_t *kClassName = L"Hd2OcrTextOverlayWnd";

// 屏幕底部中央固定位置（P2 先固定；后续可配置/跟随游戏聊天框）。
constexpr int kWidth = 1000;
constexpr int kHeight = 78;
constexpr int kBottomMargin = 150;
// 浮层整体透明度（LWA_ALPHA）：降低遮挡感，255=不透明。当前 170（半透仍清晰可读）。
constexpr BYTE kAlpha = 170;

} // namespace

TextOverlay::~TextOverlay()
{
    if (hwnd_ != nullptr)
    {
        DestroyWindow(hwnd_); // WM_DESTROY 中释放 font_
        hwnd_ = nullptr;
    }
    font_ = nullptr;
    visible_ = false;
}

bool TextOverlay::Create(HINSTANCE hInstance)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        printf("[overlay] RegisterClass failed lastError=%lu\n", GetLastError());
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                            kClassName, L"", WS_POPUP, (screenW - kWidth) / 2,
                            screenH - kBottomMargin - kHeight, kWidth, kHeight, nullptr, nullptr,
                            hInstance, this);
    if (hwnd_ == nullptr)
    {
        printf("[overlay] CreateWindowEx failed lastError=%lu\n", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, kAlpha, LWA_ALPHA);
    ShowWindow(hwnd_, SW_HIDE); // 初始隐藏，仅打字态显示
    printf("[overlay] created hwnd=%p (%dx%d bottom-center, hidden)\n", hwnd_, kWidth, kHeight);
    fflush(stdout);
    return true;
}

void TextOverlay::Show()
{
    if (hwnd_ != nullptr && !visible_)
    {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
}

void TextOverlay::Hide()
{
    if (hwnd_ != nullptr && visible_)
    {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

void TextOverlay::SetText(const std::wstring &text)
{
    if (hwnd_ == nullptr)
    {
        return;
    }
    text_ = text;
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_); // 同线程调用，立即重绘（低延迟）
}

LRESULT CALLBACK TextOverlay::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *self = reinterpret_cast<TextOverlay *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
    case WM_CREATE:
    {
        const auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        auto *owner = reinterpret_cast<TextOverlay *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
        // 支持中文的大号字体；失败则 WM_PAINT 回退默认 GUI 字体。
        owner->font_ = CreateFontW(-30, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // 背景由 WM_PAINT 全量重绘，避免闪烁
    case WM_PAINT:
        if (self == nullptr)
        {
            break;
        }
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH bg = CreateSolidBrush(RGB(0x14, 0x14, 0x1E));
            FillRect(dc, &rect, bg);
            DeleteObject(bg);
            if (!self->text_.empty())
            {
                HFONT font = self->font_ != nullptr
                                 ? self->font_
                                 : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
                RECT textRect = rect;
                InflateRect(&textRect, -18, 0);
                DrawTextW(dc, self->text_.c_str(), -1, &textRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dc, oldFont);
            }
            EndPaint(hwnd, &paint);
        }
        return 0;
    case WM_DESTROY:
        if (self != nullptr && self->font_ != nullptr)
        {
            DeleteObject(self->font_);
            self->font_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
