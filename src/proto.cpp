// hd2-ocr-input prototype — P3：Enter 两段式发送闭环。
//
// P2（迷你浮层）之上接入发送闭环：非组字态 Enter = 发送（读承载窗文本 → 隐藏浮层/
// 承载窗并还焦游戏 → 校验前台=HD2 → SendInput 整句 + 补 Enter）；非组字态 Esc = 取消
// 退出。组字中 Enter/Esc 仍由 IME 处理（确认候选/取消组合），carrier 子类已区分。
//
// bugfix：发送/Esc 属"应用内退出"，需同步热键源的 toggle 状态（SyncSensingClosed），
// 否则下一次 F8 会因内部状态残留需按两次才能进入打字态。
//
//   proto [--duration <ms>] [--alpha <0-255>]   守护模式（F8 toggle；默认 alpha=0 全透明）
//   proto fg / proto inject ...                  复用 P0 子命令
//   proto help
//
// 编译：build.cmd（vswhere + cl，零第三方依赖）。产物 proto.exe。

#include "carrier.h"
#include "fgutil.h"
#include "floattext.h"
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

// 主控：接收感知/承载窗事件，驱动浮层与发送闭环。P3 接入完成（P4 感知层替换热键）。
struct AppController
{
    CarrierWindow carrier;
    TextOverlay overlay;
    HWND gameHwnd = nullptr;
    HotkeySensingSource *sensingSource = nullptr; // 热键占位源：应用内退出后同步其状态
    bool inChat = false;

    void OnChatOpen()
    {
        if (inChat)
        {
            return;
        }
        inChat = true;
        gameHwnd = GetForegroundWindow(); // 按 F8 时前台应为游戏（聊天框已打开）
        printf("[app] chat-open  gameHwnd=%p -> carrier.show+focus + overlay.show\n", gameHwnd);
        fflush(stdout);
        carrier.ShowAndFocus(gameHwnd);
        overlay.Show();
    }

    // 退出打字态（不发送）：隐藏浮层与承载窗，前台还给游戏。
    void DoExitChat()
    {
        if (!inChat)
        {
            return;
        }
        inChat = false;
        printf("[app] exit-chat -> overlay.hide + carrier.hide+restore\n");
        fflush(stdout);
        overlay.Hide();
        carrier.HideAndRestoreFocus();
        SyncSensingClosed(); // 幂等：覆盖 F8 正常退出与一切应用内退出路径
    }

    // 应用内退出（发送/Esc）后同步热键源内部 toggle 状态，防止下次 F8 需按两次。
    void SyncSensingClosed()
    {
        if (sensingSource != nullptr)
        {
            sensingSource->ResetToClosed();
        }
    }

    void OnChatClose()
    {
        DoExitChat(); // F8 第二次按下 = 放弃退出
    }

    // 非组字态 Esc → 取消退出。
    void OnCancelRequested()
    {
        printf("[app] cancel (Esc)\n");
        fflush(stdout);
        DoExitChat();
    }

    // 非组字态 Enter → 发送闭环。
    void OnSendRequested(std::wstring text)
    {
        if (!inChat)
        {
            return;
        }
        printf("[app] send: \"%ls\" (%zu units)\n", text.c_str(), text.size());
        fflush(stdout);
        // 先退出视觉打字态并还焦游戏，再做防误投校验与注入。
        overlay.Hide();
        carrier.HideAndRestoreFocus();
        inChat = false;
        SyncSensingClosed(); // 发送属应用内退出，热键 toggle 状态需复位
        // InjectText 内部再校验前台=HD2（不匹配即拒绝、不补发 Enter），失败即中止。
        const int result = InjectText(text, /*submit=*/true, /*delayMs=*/0);
        printf("[app] send result=%d (%s)\n", result, result == 0 ? "ok" : "failed-aborted");
        fflush(stdout);
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

// 守护模式：创建承载窗与浮层 → 绑定文本/发送/取消回调 → 注册 F8 → 泵消息。
int RunDaemon(BYTE windowAlpha, DWORD maxDurationMs)
{
    AppController controller;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!controller.carrier.Create(instance, windowAlpha))
    {
        printf("[daemon] carrier create failed\n");
        return 9;
    }
    if (!controller.overlay.Create(instance))
    {
        printf("[daemon] overlay create failed\n");
        return 9;
    }
    // Edit 已上屏文本变化 → 刷新迷你浮层。
    controller.carrier.SetOnTextChanged(
        [&controller](std::wstring text) { controller.overlay.SetText(std::move(text)); });
    // 非组字态 Enter → 发送闭环；Esc → 取消退出。
    controller.carrier.SetOnSendRequested(
        [&controller](std::wstring text) { controller.OnSendRequested(std::move(text)); });
    controller.carrier.SetOnCancelRequested([&controller]() { controller.OnCancelRequested(); });

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
    controller.sensingSource = &sensing; // 供应用内退出（发送/Esc）时同步 toggle 状态

    printf("[daemon] running (max=%lu ms) — F8: enter/exit typing, Enter: send, Esc: cancel, "
           "Ctrl+C: quit\n",
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
                controller.overlay.Hide();
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
    controller.overlay.Hide();
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
