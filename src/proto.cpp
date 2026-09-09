// hd2-ocr-input prototype — P4-2：像素特征感知（PixelSensingSource + 四态判别）。
//
// P4-1（capture/feature/sniff）之上：
//  - feature 增加区域亮度分层与四态判别（Closed/Open/NoPanel，9 张真机样本离线验证）；
//  - 新增 PixelSensingSource：前台为 HD2 时按客户区比例定位聊天框 ROI → capture →
//    亮度分层四态 → 去抖 → ChatOpen/ChatClose 事件（转场/CG 的 NoPanel 不动作）；
//  - classify：读像素 bin 复算四态（供 9 张样本离线回归验证判别实现）；
//  - pixeldemo：真机观察自动感知（打开/关闭游戏聊天框看事件输出）。
//
//   proto classify <roi.bin>              [w][h] 头 + BGRA32 像素 → 打印四态分类
//   proto pixeldemo [--duration <ms>]    运行像素感知源打印事件（观察自动检测）
//   proto sniff ... / fg / inject / daemon(守护 F8)  见各注释
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

void PrintUsage()
{
    printf("usage:\n"
           "  proto [--duration <ms>] [--alpha <0-255>]   daemon: F8 toggles chat open/close;\n"
           "                                              alpha 0=full transparent (default),\n"
           "                                              raise (e.g. 220) to see carrier/IME\n"
           "  proto pixeldemo [--duration <ms>]           run pixel sensing, print open/close events\n"
           "  proto classify <roi.bin>                    classify a dumped ROI frame (4-state)\n"
           "  proto sniff [--x <px>] [--y <px>] [--w <px>] [--h <px>]\n"
           "             [--interval <ms>] [--count <n>]  observe ROI pixel features (calibration)\n"
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
        SyncSensingClosed(); // 幂等：覆盖 F8 正常退出与一切应用内退出路径
    }

    // 应用内退出（发送/Esc）后同步感知源状态（热键源复位 toggle；像素源加抑制期）。
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
        SyncSensingClosed(); // 发送属应用内退出，感知/热键状态需复位
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
