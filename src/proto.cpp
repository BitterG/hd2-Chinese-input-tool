// hd2-ocr-input prototype — P4-3：守护 --auto（Enter 直呼出 + 像素冗余）。
//
//   proto --auto [--duration <ms>] [--alpha <0-255>]
//     --auto 交互（进入零延迟、跟手）：
//       WH_KEYBOARD_LL 监听 Enter（只监听不吞键）：前台=HD2 且非打字态按下 Enter
//       → 主线程延迟 ~40ms（让游戏先打开聊天框，避免抢焦竞态）→ 进入打字态。
//       发送补发的 Enter（SendInput）也会被钩子捕获 → 发送后冷却窗（400ms）内忽略，
//       避免"发送成功后又自动呼出"。
//       Enter/Esc/发送等闭环逻辑同 F8 模式；F8 = 手动兜底切换。
//       像素源保留为冗余触发（主控 guard 去重；覆盖聊天键改绑等情况）。
//   非 --auto 保持原 F8 toggle 占位模式。
//
// 抑制时长参数化（支持连发）：发送成功 → 短抑制（300ms）；Esc/F8 → 长抑制（1200ms）。
//
//   proto classify <roi.bin> / pixeldemo / sniff / fg / inject  见各函数注释
//
// 编译：build.cmd（vswhere + cl，零第三方依赖）。产物 proto.exe。

#include "capture.h"
#include "carrier.h"
#include "feature.h"
#include "fgutil.h"
#include "floattext.h"
#include "inject.h"
#include "pixel.h"
#include "sensing.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

// 像素源事件 / Enter 直呼出 → 主线程投递消息（跨线程安全地驱动 UI/承载窗）。
constexpr UINT kMsgPixelOpen = WM_APP + 0x10;
constexpr UINT kMsgPixelClose = WM_APP + 0x11;
constexpr UINT kMsgEnterOpen = WM_APP + 0x12;
constexpr int kAutoF8Id = 1;
// Enter 直呼出的抢焦竞态窗口：让游戏先处理 Enter 打开聊天框，再进入打字态。
constexpr DWORD kEnterOpenDelayMs = 40;
// 发送后 Enter 直呼出冷却窗：SendInput 补发的 Enter 会被本钩子捕获，
// 冷却窗内忽略之，避免"发送成功后又自动呼出"；玩家连发在此后按 Enter 即可。
constexpr unsigned long long kEnterReopenCooldownMs = 400;

// Enter 直呼出钩子状态（--auto 时由主线程安装/清理；回调投递主线程消息）。
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
           "  proto [--auto] [--duration <ms>] [--alpha <0-255>]   daemon.\n"
           "       --auto: press Enter (in-game chat key) to instantly open typing UI;\n"
           "               F8 = manual toggle fallback; pixel sensing kept as redundancy\n"
           "       (default: F8 toggle placeholder sensing)\n"
           "       alpha 0=full transparent (default); raise (e.g. 220) to see carrier/IME\n"
           "  proto pixeldemo [--duration <ms>]           run pixel sensing, print open/close events\n"
           "  proto classify <roi.bin>                    classify a dumped ROI frame (4-state)\n"
           "  proto sniff [--x <px>] [--y <px>] [--w <px>] [--h <px>]\n"
           "             [--interval <ms>] [--count <n>]  observe ROI pixel features (calibration)\n"
           "  proto fg                                     print foreground diagnostics\n"
           "  proto inject <text...> [--enter] [--delay <ms>]   SendInput unicode inject\n"
           "  proto inject --file <utf8-path> [--enter] [--delay <ms>]\n"
           "  proto help\n");
}

// 主控：接收感知/承载窗事件，驱动浮层与发送闭环（必须在主线程调用——UI 操作）。
struct AppController
{
    // 应用内退出后的感知抑制时长：
    //   Esc/F8 手动退出：聊天框可能仍开，需较长抑制防"取消后立即自动重进"循环；
    //   发送成功：聊天框已关，只需短抑制防帧残留，允许快速连发再入。
    static constexpr unsigned long long kExitSuppressMs = 1200;
    static constexpr unsigned long long kSendSuppressMs = 300;

    CarrierWindow carrier;
    TextOverlay overlay;
    HWND gameHwnd = nullptr;
    ISensingSource *sensingSource = nullptr; // 当前感知源（热键占位 或 像素源）
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
        SyncSensingClosed(kExitSuppressMs); // 手动/取消类退出：长抑制防自动重进
    }

    // 应用内退出后同步感知源状态（热键源复位 toggle；像素源按 suppressMs 施加抑制期）。
    void SyncSensingClosed(unsigned long long suppressMs)
    {
        if (sensingSource != nullptr)
        {
            sensingSource->ResetToClosed(suppressMs);
        }
    }

    void OnChatClose()
    {
        DoExitChat(); // 感知/热键触发关闭
    }

    // F8 手动兜底切换：基于实际状态（规避热键内部 toggle 与主控状态脱节）。
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
        SyncSensingClosed(kExitSuppressMs);
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
        SyncSensingClosed(kSendSuppressMs); // 发送成功：短抑制，允许快速连发
        lastSendMs = GetTickCount64();      // 供 Enter 直呼出冷却（SendInput 补发 Enter）
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

// sniff：抓取 ROI 并打印像素特征（平均色/采样点/与上一帧的差异），供聊天框定标观察。
int RunSniff(int argc, wchar_t **argv, int argStart)
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    DWORD intervalMs = 250;
    DWORD maxCount = 0;
    for (int i = argStart; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        int *target = nullptr;
        if (arg == L"--x")
        {
            target = &x;
        }
        else if (arg == L"--y")
        {
            target = &y;
        }
        else if (arg == L"--w")
        {
            target = &w;
        }
        else if (arg == L"--h")
        {
            target = &h;
        }
        else if (arg == L"--interval" && i + 1 < argc)
        {
            ++i;
            intervalMs = static_cast<DWORD>(_wtoi(argv[i]));
        }
        else if (arg == L"--count" && i + 1 < argc)
        {
            ++i;
            maxCount = static_cast<DWORD>(_wtoi(argv[i]));
        }
        if (target != nullptr && i + 1 < argc)
        {
            ++i;
            *target = _wtoi(argv[i]);
        }
    }
    if (w <= 0 || h <= 0)
    {
        // 默认屏幕底部中央区域（贴近常见聊天框位置），便于真机直接观察。
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (w <= 0)
        {
            w = 880;
        }
        if (h <= 0)
        {
            h = 220;
        }
        if (x == 0 && y == 0)
        {
            x = (screenW - w) / 2;
            y = screenH - h - 60;
        }
    }
    printf("sniff: roi=(%d,%d %dx%d) interval=%lu ms count=%lu — Ctrl+C to stop\n", x, y, w, h,
           intervalMs, maxCount);
    fflush(stdout);

    CapturedFrame prev;
    const DWORD started = GetTickCount();
    for (DWORD frame = 0; maxCount == 0 || frame < maxCount; ++frame)
    {
        CapturedFrame cur;
        if (!CaptureScreenRegion(x, y, w, h, cur))
        {
            printf("sniff: capture failed lastError=%lu\n", GetLastError());
            return 10;
        }
        uint32_t avg = 0;
        feature::AverageColor(cur, avg);
        uint32_t tl = 0;
        uint32_t mid = 0;
        uint32_t br = 0;
        feature::SamplePixel(cur, 0, 0, tl);
        feature::SamplePixel(cur, w / 2, h / 2, mid);
        feature::SamplePixel(cur, w - 1, h - 1, br);
        printf("[%5lu ms] avg=#%02X%02X%02X tl=#%02X%02X%02X mid=#%02X%02X%02X br=#%02X%02X%02X",
               GetTickCount() - started, feature::R(avg), feature::G(avg), feature::B(avg),
               feature::R(tl), feature::G(tl), feature::B(tl), feature::R(mid), feature::G(mid),
               feature::B(mid), feature::R(br), feature::G(br), feature::B(br));
        if (!prev.empty())
        {
            double meanDiff = 0.0;
            double changed = 0.0;
            feature::FrameDiff(prev, cur, meanDiff, changed);
            printf(" diff=%.1f changed=%.1f%%", meanDiff, changed * 100.0);
        }
        else
        {
            printf(" (first frame)");
        }
        printf("\n");
        fflush(stdout);
        prev = std::move(cur);
        if (maxCount == 0 || frame + 1 < maxCount)
        {
            Sleep(intervalMs);
        }
    }
    return 0;
}

// classify：读 [int32 w][int32 h][w*h*BGRA32] 的 bin，复算四态分类（离线回归用）。
int RunClassify(int argc, wchar_t **argv, int argStart)
{
    if (argStart >= argc)
    {
        printf("classify: need a .bin path\n");
        return 11;
    }
    const std::wstring path = argv[argStart];
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
    {
        printf("classify: cannot open %ls\n", path.c_str());
        return 11;
    }
    int w = 0;
    int h = 0;
    size_t read = fread(&w, sizeof(int), 1, file);
    read += fread(&h, sizeof(int), 1, file);
    if (read != 2 || w <= 0 || h <= 0 || w > 10000 || h > 10000)
    {
        fclose(file);
        printf("classify: bad header\n");
        return 11;
    }
    CapturedFrame frame;
    frame.width = w;
    frame.height = h;
    frame.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    const size_t want = frame.pixels.size() * sizeof(uint32_t);
    const size_t got = fread(frame.pixels.data(), 1, want, file);
    fclose(file);
    if (got != want)
    {
        printf("classify: short read %zu/%zu\n", got, want);
        return 11;
    }
    const int detectW = static_cast<int>(frame.width * 0.62);
    feature::PanelFeatures features;
    feature::MeasurePanelFeatures(frame, 0, 0, detectW, frame.height, features);
    const feature::PanelState state = feature::ClassifyPanel(features);
    const char *name = state == feature::PanelState::Closed   ? "Closed"
                       : state == feature::PanelState::Open   ? "Open"
                                                               : "NoPanel";
    printf("classify: %dx%d detectW=%d runMed=%.2f M150=%.2f%% iconW240=%.2f%% -> %s\n", w, h,
           detectW, features.runMedRatio, features.pctM150, features.pctIconW240, name);
    return 0;
}

// pixeldemo：运行像素感知源，打印 ChatOpen/ChatClose 事件（真机观察自动检测）。
int RunPixelDemo(int argc, wchar_t **argv, int argStart)
{
    DWORD durationMs = 60000;
    for (int i = argStart; i < argc; ++i)
    {
        if (argv[i][0] != L'\0' && wcscmp(argv[i], L"--duration") == 0 && i + 1 < argc)
        {
            ++i;
            durationMs = static_cast<DWORD>(_wtoi(argv[i]));
        }
    }
    printf("pixeldemo: watching HD2 chat panel for %lu ms — open/close the in-game chat box\n",
           durationMs);
    fflush(stdout);

    PixelSensingSource pixel;
    pixel.Start([](bool chatOpen) {
        printf("[demo] %s\n", chatOpen ? "CHAT-OPEN" : "CHAT-CLOSE");
        fflush(stdout);
    });
    const DWORD started = GetTickCount();
    while (durationMs == 0 || GetTickCount() - started < durationMs)
    {
        Sleep(100);
    }
    pixel.Stop();
    return 0;
}

// 守护模式：创建承载窗与浮层 → 绑定事件 → 注册 Enter 钩子/F8 → 泵消息。
// autoMode=true：Enter 直呼出（WH_KEYBOARD_LL 监听）+ 像素源冗余 + F8 手动兜底。
// autoMode=false：F8 toggle 占位感知（热键源）。
int RunDaemon(bool autoMode, BYTE windowAlpha, DWORD maxDurationMs)
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
    PixelSensingSource pixelSensing; // autoMode 时启用
    HotkeySensingSource hotkeySensing;
    bool autoRegistered = false;
    HHOOK enterHook = nullptr;

    if (autoMode)
    {
        g_hookMainThreadId = mainThreadId;
        controller.sensingSource = &pixelSensing;
        pixelSensing.Start([mainThreadId](bool chatOpen) {
            // 轮询线程回调 → 投递主线程消息，由消息循环驱动 UI。
            PostThreadMessageW(mainThreadId, chatOpen ? kMsgPixelOpen : kMsgPixelClose, 0, 0);
        });
        if (!pixelSensing.running())
        {
            return 8;
        }
        // Enter 直呼出：只监听不吞键，游戏正常收到 Enter 打开聊天框。
        enterHook = SetWindowsHookExW(WH_KEYBOARD_LL, EnterOpenHookProc, instance, 0);
        if (enterHook == nullptr)
        {
            printf("[daemon] Enter hook install failed lastError=%lu\n", GetLastError());
        }
        if (RegisterHotKey(nullptr, kAutoF8Id, MOD_NOREPEAT, VK_F8))
        {
            autoRegistered = true;
            printf("[daemon] F8 manual-toggle fallback registered\n");
            fflush(stdout);
        }
        printf("[daemon] auto on — press Enter (game chat key) to open typing UI instantly, "
               "F8 = manual toggle\n");
        fflush(stdout);
    }
    else
    {
        controller.sensingSource = &hotkeySensing;
        hotkeySensing.Start([&controller](bool chatOpen) {
            if (chatOpen)
            {
                controller.OnChatOpen();
            }
            else
            {
                controller.OnChatClose();
            }
        });
        if (!hotkeySensing.running())
        {
            return 8;
        }
    }

    printf("[daemon] running (max=%lu ms) — typing: Enter send, Esc cancel, Ctrl+C quit\n",
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
                goto done; // 统一清理
            }
            if (message.message == kMsgPixelOpen || message.message == kMsgPixelClose)
            {
                if (message.message == kMsgPixelOpen)
                {
                    controller.OnChatOpen();
                }
                else
                {
                    controller.OnChatClose();
                }
            }
            else if (message.message == kMsgEnterOpen)
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
                if (autoMode)
                {
                    controller.ManualToggle();
                }
                else
                {
                    hotkeySensing.HandleHotkey(message.wParam);
                }
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
done:
    if (autoMode)
    {
        if (enterHook != nullptr)
        {
            UnhookWindowsHookEx(enterHook);
        }
        pixelSensing.Stop();
        if (autoRegistered)
        {
            UnregisterHotKey(nullptr, kAutoF8Id);
        }
        g_hookMainThreadId = 0;
    }
    else
    {
        hotkeySensing.Stop();
    }
    controller.overlay.Hide();
    controller.carrier.HideAndRestoreFocus();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    // 守护参数（任意命令位置）：--auto / --duration / --alpha。
    bool autoMode = false;
    BYTE windowAlpha = 0;
    DWORD maxDurationMs = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--auto")
        {
            autoMode = true;
        }
        else if (arg == L"--alpha" && i + 1 < argc)
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
        if (command == L"classify")
        {
            return RunClassify(argc, argv, 2);
        }
        if (command == L"pixeldemo")
        {
            return RunPixelDemo(argc, argv, 2);
        }
        if (command == L"sniff")
        {
            return RunSniff(argc, argv, 2);
        }
        if (command == L"inject")
        {
            return RunInject(argc, argv, 2);
        }
        if (command == L"--auto" || command == L"--duration" || command == L"--alpha")
        {
            return RunDaemon(autoMode, windowAlpha, maxDurationMs);
        }
        printf("proto: unknown command '%ls'\n", command.c_str());
        PrintUsage();
        return 0;
    }
    return RunDaemon(autoMode, windowAlpha, maxDurationMs);
}
