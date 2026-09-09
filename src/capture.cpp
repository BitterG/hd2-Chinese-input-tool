#include "capture.h"

// GDI BitBlt + GetDIBits 区域截屏（零第三方依赖）。游戏窗口化/无边框时屏幕 DC 直接可见。

bool CaptureScreenRegion(int x, int y, int w, int h, CapturedFrame &out)
{
    if (w <= 0 || h <= 0)
    {
        return false;
    }
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        return false;
    }
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, w, h);
    if (memDc == nullptr || bitmap == nullptr)
    {
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        if (memDc != nullptr)
        {
            DeleteDC(memDc);
        }
        ReleaseDC(nullptr, screenDc);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    const BOOL blitted = BitBlt(memDc, 0, 0, w, h, screenDc, x, y, SRCCOPY | CAPTUREBLT);

    out.width = w;
    out.height = h;
    out.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    const int lines = blitted ? GetDIBits(memDc, bitmap, 0, h, out.pixels.data(), &info,
                                          DIB_RGB_COLORS)
                              : 0;

    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return lines == h;
}
