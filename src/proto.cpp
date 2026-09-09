// hd2-ocr-input prototype — P1：全屏透明承载窗 + IME 激活验证。
//
// P0（热键占位感知 + 地基）之上接入 CarrierWindow：F8 进入打字态 = 显示全屏透明
// 承载窗并拿焦（内嵌标准 Edit 承载系统 IME）；F8 退出 = 隐藏承载窗并把前台还给游戏。
// 验证目标：系统中文输入法在承载窗上能正常组字、候选窗浮出。
//
//   proto [--duration <ms>] [--alpha <0-255>]   守护模式（F8 toggle；默认 alpha=0 全透明）
//   proto fg / proto inject ...                  复用 P0 子命令
//   proto help
//
// 编译：build.cmd（vswhere + cl，零第三方依赖）。产物 proto.exe。

#include "carrier.h"
#include "fgutil.h"
#include "inject.h"
#include "sensing.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void PrintUsage()
{
    printf("usage:\n"
           "  proto [--duration <ms>] [--alpha <0-255>]   daemon: F8 toggles chat open/close;\n"
           "                                              alpha 0=full transparent (default),\n"
           "                                              raise (e.g. 220) to see carrier/IME\n"
           "  proto fg                                     print foreground diagnostics\n"
           "  proto inject <text...> [--enter] [--delay <ms>]   SendInput unicode inject\n"
           "  proto inject --file <utf8-path> [--enter] [--delay <ms>]\n"
           "  proto help\n");
}

// 主控：接收感知事件，驱动承载窗。P1 接入完成（P2/P3 追加浮层与发送闭环）。
struct AppController
{
    CarrierWindow carrier;
    HWND gameHwnd = nullptr;
    bool inChat = false;

    void OnChatOpen()
    {
        if (inChat)
        {
            return;
        }
        inChat = true;
        gameHwnd = GetForegroundWindow(); // 按 F8 时前台应为游戏（聊天框已打开）
        printf("[app] chat-open  gameHwnd=%p -> carrier.show+focus\n", gameHwnd);
        fflush(stdout);
        carrier.ShowAndFocus(gameHwnd);
    }

    void OnChatClose()
    {
        if (!inChat)
        {
            return;
        }
        inChat = false;
        printf("[app] chat-close -> carrier.hide+restore\n");
        fflush(stdout);
        carrier.HideAndRestoreFocus();
    }
};

// 注入子命令（参数语义与 probe inject 一致，P0 已验）。
int RunInject(int argc, wchar_t **argv, int argStart)
{
    bool submit = false;
    bool fromFile = false;
    DWORD delayMs = 0;
    std::wstring filePath;
    std::vector<std::wstring> words;
    for (int i = argStart; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--enter")
        {
            submit = true;
        }
        else if (arg == L"--delay")
        {
            if (i + 1 < argc)
            {
                ++i;
                delayMs = static_cast<DWORD>(_wtoi(argv[i]));
            }
        }
        else if (arg == L"--file")
        {
            fromFile = true;
            if (i + 1 < argc)
            {
                filePath = argv[++i];
            }
        }
        else
        {
            words.push_back(arg);
        }
    }
    std::wstring text;
    if (fromFile)
    {
        text = ReadUtf8File(filePath.c_str());
        if (text.empty())
        {
            printf("inject: failed to read --file %ls\n", filePath.c_str());
            return 7;
        }
    }
    else
    {
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (i > 0)
            {
                text += L' ';
            }
            text += words[i];
        }
    }
    if (text.empty())
    {
        printf("inject: empty text\n");
        return 7;
    }
    return InjectText(text, submit, delayMs);
}

// 守护模式：创建承载窗（隐藏）→ 注册 F8 感知 → 泵消息；--duration 超时兜底退出。
int RunDaemon(BYTE windowAlpha, DWORD maxDurationMs)
{
    AppController controller;
    if (!controller.carrier.Create(GetModuleHandleW(nullptr), windowAlpha))
    {
        printf("[daemon] carrier create failed\n");
        return 9;
    }

    HotkeySensingSource sensing;
    sensing.Start([&controller](bool chatOpen) {
        if (chatOpen)
        {
            controller.OnChatOpen();
        }
        else
        {
            controller.OnChatClose();
        }
    });
    if (!sensing.running())
    {
        sensing.Stop();
        return 8;
    }

    printf("[daemon] running (max=%lu ms) — press F8 to toggle typing state, Ctrl+C to quit\n",
           maxDurationMs);
    fflush(stdout);

    // PeekMessage + Sleep(10) 轮询（沿用探针模式：可控退出、不依赖阻塞唤醒）。
    const DWORD started = GetTickCount();
    MSG message{};
    for (;;)
    {
        if (maxDurationMs > 0 && GetTickCount() - started > maxDurationMs)
        {
            printf("[daemon] timed out after %lu ms\n", maxDurationMs);
            break;
        }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                printf("[daemon] WM_QUIT received\n");
                sensing.Stop();
                controller.carrier.HideAndRestoreFocus();
                return 0;
            }
            if (message.message == WM_HOTKEY)
            {
                sensing.HandleHotkey(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
    sensing.Stop();
    controller.carrier.HideAndRestoreFocus();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2)
    {
        const std::wstring command = argv[1];
        if (command == L"help" || command == L"--help")
        {
            PrintUsage();
            return 0;
        }
        if (command == L"fg")
        {
            return PrintForegroundDiagnostics();
        }
        if (command == L"inject")
        {
            return RunInject(argc, argv, 2);
        }
        if (command == L"--duration" || command == L"--alpha")
        {
            BYTE windowAlpha = 0;
            DWORD maxDurationMs = 0;
            for (int i = 1; i < argc; ++i)
            {
                const std::wstring arg = argv[i];
                if (arg == L"--alpha" && i + 1 < argc)
                {
                    ++i;
                    const int value = _wtoi(argv[i]);
                    windowAlpha = static_cast<BYTE>(value < 0 ? 0 : (value > 255 ? 255 : value));
                }
                else if (arg == L"--duration" && i + 1 < argc)
                {
                    ++i;
                    maxDurationMs = static_cast<DWORD>(_wtoi(argv[i]));
                }
            }
            return RunDaemon(windowAlpha, maxDurationMs);
        }
        printf("proto: unknown command '%ls'\n", command.c_str());
        PrintUsage();
        return 0;
    }
    return RunDaemon(0, 0);
}
