// 前台窗口识别工具 —— 从 hd2-input-probe/probe.cpp 提取复用（真机已验证），
// 去掉命令解析，保留纯函数与 fg 诊断。

#include "fgutil.h"

#include <dwmapi.h>
#include <cstdio>

namespace {

// 前台窗口的进程名，取可执行文件名小写。
bool ForegroundProcessNameImpl(std::wstring &name, DWORD &pid)
{
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr)
    {
        return false;
    }
    DWORD threadId = GetWindowThreadProcessId(foreground, &pid);
    if (threadId == 0 || pid == 0)
    {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        return false;
    }
    wchar_t image[MAX_PATH] = L"";
    DWORD size = MAX_PATH;
    QueryFullProcessImageNameW(process, 0, image, &size);
    CloseHandle(process);
    if (image[0] == L'\0')
    {
        return false;
    }
    const wchar_t *slash = wcsrchr(image, L'\\');
    name = (slash != nullptr) ? (slash + 1) : image;
    for (auto &ch : name)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            ch += L'a' - L'A';
        }
    }
    return true;
}

} // namespace

bool ForegroundProcessName(std::wstring &name, DWORD &pid)
{
    return ForegroundProcessNameImpl(name, pid);
}

bool IsForegroundHd2Like(HWND *outHwnd, std::wstring *outTitle)
{
    HWND foreground = GetForegroundWindow();
    if (outHwnd != nullptr)
    {
        *outHwnd = foreground;
    }
    if (foreground == nullptr)
    {
        return false;
    }
    wchar_t titleBuffer[512] = L"";
    GetWindowTextW(foreground, titleBuffer, 512);
    std::wstring title = titleBuffer;
    if (outTitle != nullptr)
    {
        *outTitle = title;
    }

    std::wstring processName;
    DWORD pid = 0;
    const bool gotProcess = ForegroundProcessName(processName, pid);
    if (gotProcess && processName == L"helldivers2.exe")
    {
        return true;
    }
    for (auto &ch : title)
    {
        if (ch >= L'a' && ch <= L'z')
        {
            ch -= L'a' - L'A';
        }
    }
    return title.find(L"HELLDIVERS") != std::wstring::npos;
}

int PrintForegroundDiagnostics()
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr)
    {
        printf("fg: no foreground window\n");
        return 1;
    }
    wchar_t title[512] = L"";
    wchar_t className[256] = L"";
    GetWindowTextW(foreground, title, 512);
    GetClassNameW(foreground, className, 256);

    DWORD pid = 0;
    const DWORD threadId = GetWindowThreadProcessId(foreground, &pid);

    std::wstring processName;
    const bool gotProcess = ForegroundProcessName(processName, pid);

    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    printf("fg: hwnd=%p pid=%lu tid=%lu\n", foreground, pid, threadId);
    printf("    title   = %ls\n", title);
    printf("    class   = %ls\n", className);
    printf("    process = %ls\n", gotProcess ? processName.c_str() : L"<unknown>");
    printf("    visible=%d minimized=%d cloaked=%d\n", IsWindowVisible(foreground) ? 1 : 0,
           IsIconic(foreground) ? 1 : 0, cloaked ? 1 : 0);
    printf("    hd2-like=%d\n", IsForegroundHd2Like(nullptr, nullptr) ? 1 : 0);
    return 0;
}
