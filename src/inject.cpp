// SendInput Unicode 注入 —— 从 hd2-input-probe/probe.cpp 提取复用（真机已验证）。

#include "inject.h"
#include "fgutil.h"

#include <cstdio>
#include <vector>

namespace {

std::wstring Utf8ToWide(const char *utf8)
{
    if (utf8 == nullptr || *utf8 == '\0')
    {
        return L"";
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0)
    {
        return L"";
    }
    std::wstring wide(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), len);
    return wide;
}

} // namespace

std::wstring ReadUtf8File(const wchar_t *path)
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || file == nullptr)
    {
        return L"";
    }
    std::string bytes;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        bytes.append(buffer, read);
    }
    fclose(file);
    return Utf8ToWide(bytes.c_str());
}

int InjectText(const std::wstring &text, bool submit, DWORD delayMs)
{
    if (delayMs > 0)
    {
        printf("inject: waiting %lu ms — switch to the target window now\n", delayMs);
        fflush(stdout);
        Sleep(delayMs);
    }
    HWND foreground = nullptr;
    std::wstring title;
    if (!IsForegroundHd2Like(&foreground, &title))
    {
        printf("inject: REJECTED — foreground does not match HELLDIVERS/helldivers2.exe\n");
        printf("        (title was: %ls)\n", title.c_str());
        return 3;
    }
    printf("inject: target ok — hwnd=%p title=%ls\n", foreground, title.c_str());

    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2 + 2);
    for (wchar_t unit : text)
    {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = static_cast<WORD>(unit);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    if (submit)
    {
        INPUT enterDown{};
        enterDown.type = INPUT_KEYBOARD;
        enterDown.ki.wScan = 0x1C; // Enter 扫描码
        enterDown.ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs.push_back(enterDown);

        INPUT enterUp = enterDown;
        enterUp.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(enterUp);
    }

    UINT inserted = 0;
    for (size_t i = 0; i < inputs.size();)
    {
        const size_t remaining = inputs.size() - i;
        const UINT count = remaining > 32 ? 32 : static_cast<UINT>(remaining);
        const UINT done = SendInput(count, inputs.data() + i, sizeof(INPUT));
        inserted += done;
        i += count;
        if (done != count)
        {
            printf("inject: SendInput short write (%u/%u, lastError=%lu)\n", done, count,
                   GetLastError());
            break;
        }
        Sleep(5); // 给游戏聊天框消化时间，避免一次洪峰丢事件
    }

    printf("inject: utf16-units=%zu events-ok=%u/%zu submit=%d\n", text.size(), inserted,
           inputs.size(), submit ? 1 : 0);
    return inserted == inputs.size() ? 0 : 4;
}
