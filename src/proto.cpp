// hd2-ocr-input prototype — HD2 中文输入工具（点开即用）。
//
//   直接运行 proto.exe 即进入可用状态（Enter 直呼出）：
//     WH_KEYBOARD_LL 监听游戏内 Enter（不吞键）——前台=HD2 且非打字态按下 Enter →
//     延迟 ~40ms（让游戏先打开聊天框，防抢焦竞态）→ 显示透明承载窗进入打字态
//     （系统 IME 组字 + 迷你浮层看已上屏文本）。Enter 发送 / Esc 取消 / F8 手动切换。
//     发送补发的 Enter 也会被钩子捕获 → 发送后 400ms 冷却忽略，避免发送完又自动呼出。
//     玩家切走（Alt-Tab）→ 失焦自退，静默隐藏不抢前台。
//
//   可选参数：--alpha <0-255>（承载窗透明度，0=全透明，调试时可调高查看）
//             --duration <ms>（自动退出，测试用）
//   工具子命令：fg（前台诊断）/ inject（Unicode 注入，防误投）。
//
//   历史说明：早期"像素特征 + OCR 感知聊天框开/关"与 F8 toggle 占位模式已移除，
//   相关判据存 git 历史与 SENSING.md（已标注废弃）。
//
// 编译：build.cmd（vswhere + cl，零第三方依赖）。产物 proto.exe。

#include "carrier.h"
#include "fgutil.h"
#include "floattext.h"
#include "inject.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Enter 直呼出 → 主线程投递消息（跨线程安全地驱动 UI/承载窗）。
constexpr UINT kMsgEnterOpen = WM_APP + 0x12;
constexpr int kF8HotkeyId = 1;
// Enter 直呼出的抢焦竞态窗口：让游戏先处理 Enter 打开聊天框，再进入打字态。
constexpr DWORD kEnterOpenDelayMs = 40;
// 发送后 Enter 直呼出冷却窗：SendInput 补发的 Enter 会被本钩子捕获，
// 冷却窗内忽略之，避免"发送成功后又自动呼出"；玩家连发在此后按 Enter 即可。
constexpr unsigned long long kEnterReopenCooldownMs = 400;

// Enter 直呼出钩子状态（主线程安装/清理；回调投递主线程消息）。
DWORD g_hookMainThreadId = 0;

// WH_KEYBOARD_LL：前台为 HD2 且按 Enter → 通知主线程进入打字态（不吞键，放行给游戏）。
LRESULT CALLBACK EnterOpenHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == WM_KEYDOWN)
    {
        const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (info->vkCode == VK_RETURN && IsForegroundHd2Like(nullptr, nullptr) &&
            g_hookMainThreadId != 0)
        {
            // 打字态前台=承载窗（非 HD2），上面 IsForegroundHd2Like 已挡掉，不会重复呼出。
            PostThreadMessageW(g_hookMainThreadId, kMsgEnterOpen, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void PrintUsage()
{
    printf("usage:\n"
           "  proto [--alpha <0-255>] [--duration <ms>]   run (point-and-play):\n"
           "        press Enter in HELLDIVERS to instantly start typing Chinese;\n"
           "        Enter send / Esc cancel / F8 manual toggle; Alt-Tab auto quits\n"
           "  proto fg                                     print foreground diagnostics\n"
           "  proto inject <text...> [--enter] [--delay <ms>]   SendInput unicode inject\n"
           "  proto inject --file <utf8-path> [--enter] [--delay <ms>]\n"
           "  proto help\n");
}

// 主控：接收事件（Enter 直呼出/热键/承载窗），驱动浮层与发送闭环（主线程调用——UI 操作）。
struct AppController
{
    CarrierWindow carrier;
    TextOverlay overlay;
    HWND gameHwnd = nullptr;
    bool inChat = false;
    ULONGLONG lastSendMs = 0; // 最近发送完成时刻（GetTickCount64），供 Enter 呼出冷却

    void OnChatOpen()
    {
        if (inChat)
        {
            return;
        }
        inChat = true;
        gameHwnd = GetForegroundWindow(); // 进入打字态时前台应为游戏（聊天框已打开）
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
    }

    // F8 手动切换：基于实际状态（规避 toggle 状态脱节问题）。
    void ManualToggle()
    {
        printf("[app] F8 manual toggle\n");
        fflush(stdout);
        if (inChat)
        {
            DoExitChat();
        }
        else
        {
            OnChatOpen();
        }
    }

    // 非组字态 Esc → 取消退出。
    void OnCancelRequested()
    {
        printf("[app] cancel (Esc)\n");
        fflush(stdout);
        DoExitChat();
    }

    // 承载窗失去激活（玩家 Alt-Tab/点走）→ 静默退出打字态，不抢回前台。
    void OnCarrierInactive()
    {
        if (!inChat)
        {
            return;
        }
        inChat = false;
        printf("[app] carrier-inactive -> quiet exit (switched away)\n");
        fflush(stdout);
        overlay.Hide();
        carrier.HideQuiet();
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
        lastSendMs = GetTickCount64(); // 供 Enter 直呼出冷却（SendInput 补发 Enter）
        // InjectText 内部再校验前台=HD2（不匹配即拒绝、不补发 Enter），失败即中止。
        const int result = InjectText(text, /*submit=*/true, /*delayMs=*/0);
        printf("[app] send result=%d (%s)\n", result, result == 0 ? "ok" : "failed-aborted");
        fflush(stdout);
    }
};

// 注入子命令（参数语义与 probe inject 一致，真机验证过）。
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

// 守护主循环：创建承载窗与浮层 → 绑定事件 → 注册 Enter 钩子/F8 → 泵消息。
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
    // 非组字态 Enter → 发送闭环；Esc → 取消退出；失焦 → 静默退出（玩家切走）。
    controller.carrier.SetOnSendRequested(
        [&controller](std::wstring text) { controller.OnSendRequested(std::move(text)); });
    controller.carrier.SetOnCancelRequested([&controller]() { controller.OnCancelRequested(); });
    controller.carrier.SetOnInactive([&controller]() { controller.OnCarrierInactive(); });

    const DWORD mainThreadId = GetCurrentThreadId();
    g_hookMainThreadId = mainThreadId;
    // Enter 直呼出：只监听不吞键，游戏正常收到 Enter 打开聊天框。
    HHOOK enterHook = SetWindowsHookExW(WH_KEYBOARD_LL, EnterOpenHookProc, instance, 0);
    if (enterHook == nullptr)
    {
        printf("[daemon] Enter hook install failed lastError=%lu\n", GetLastError());
    }
    bool f8Registered = false;
    if (RegisterHotKey(nullptr, kF8HotkeyId, MOD_NOREPEAT, VK_F8))
    {
        f8Registered = true;
        printf("[daemon] F8 manual-toggle fallback registered\n");
        fflush(stdout);
    }
    printf("[daemon] running — press Enter in HELLDIVERS to type Chinese (Enter send / Esc "
           "cancel / F8 toggle / Ctrl+C quit)\n");
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
                goto done; // 统一清理
            }
            if (message.message == kMsgEnterOpen)
            {
                // 发送后冷却：SendInput 补发的 Enter 会被钩子捕获，冷却窗内忽略，
                // 避免"发送成功后又自动呼出"；玩家连发需在此窗之后按 Enter。
                if (GetTickCount64() - controller.lastSendMs < kEnterReopenCooldownMs)
                {
                    printf("[daemon] Enter-open ignored (send cooldown)\n");
                    fflush(stdout);
                }
                else
                {
                    // 延迟一小段：让游戏先处理 Enter 打开聊天框，避免抢焦竞态。
                    Sleep(kEnterOpenDelayMs);
                    controller.OnChatOpen();
                }
            }
            else if (message.message == WM_HOTKEY)
            {
                controller.ManualToggle();
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
done:
    if (enterHook != nullptr)
    {
        UnhookWindowsHookEx(enterHook);
    }
    if (f8Registered)
    {
        UnregisterHotKey(nullptr, kF8HotkeyId);
    }
    g_hookMainThreadId = 0;
    controller.overlay.Hide();
    controller.carrier.HideAndRestoreFocus();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    // 可选参数：--alpha / --duration（位置任意）。
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
        if (command == L"--alpha" || command == L"--duration")
        {
            return RunDaemon(windowAlpha, maxDurationMs);
        }
        printf("proto: unknown command '%ls'\n", command.c_str());
        PrintUsage();
        return 0;
    }
    return RunDaemon(windowAlpha, maxDurationMs);
}
