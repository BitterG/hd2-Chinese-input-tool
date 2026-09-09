#include "pixel.h"

#include "capture.h"
#include "feature.h"
#include "fgutil.h"

#include <cstdio>
#include <utility>

PixelSensingSource::PixelSensingSource(RoiSpec roi, int intervalMs, int debounceFrames)
    : roi_(roi), intervalMs_(intervalMs), debounceFrames_(debounceFrames)
{
}

void PixelSensingSource::Start(std::function<void(bool chatOpen)> onEvent)
{
    if (running_.load())
    {
        return;
    }
    onEvent_ = std::move(onEvent);
    stopRequested_.store(false);
    suppressOpenUntilMs_.store(0);
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    printf("[pixel] started (interval=%d ms, debounce=%d frames) — waiting for HELLDIVERS "
           "foreground\n",
           intervalMs_, debounceFrames_);
    fflush(stdout);
}

void PixelSensingSource::Stop()
{
    if (!running_.load())
    {
        return;
    }
    stopRequested_.store(true);
    if (thread_.joinable())
    {
        thread_.join();
    }
    running_.store(false);
    onEvent_ = nullptr;
    printf("[pixel] stopped\n");
    fflush(stdout);
}

void PixelSensingSource::ResetToClosed()
{
    suppressOpenUntilMs_.store(GetTickCount64() + kSuppressOpenMs);
}

void PixelSensingSource::Loop()
{
    feature::DebounceState debounce(debounceFrames_);
    bool sentOpen = false;
    while (!stopRequested_.load())
    {
        const ULONGLONG now = GetTickCount64();
        HWND game = nullptr;
        std::wstring title;
        if (IsForegroundHd2Like(&game, &title) && game != nullptr)
        {
            RECT client{};
            GetClientRect(game, &client);
            POINT origin{0, 0};
            ClientToScreen(game, &origin);
            const double clientW = static_cast<double>(client.right - client.left);
            const double clientH = static_cast<double>(client.bottom - client.top);
            if (clientW > 0.0 && clientH > 0.0)
            {
                const int rx = origin.x + static_cast<int>(clientW * roi_.relX);
                const int ry = origin.y + static_cast<int>(clientH * roi_.relY);
                const int rw = static_cast<int>(clientW * roi_.relW);
                const int rh = static_cast<int>(clientH * roi_.relH);
                CapturedFrame frame;
                if (CaptureScreenRegion(rx, ry, rw, rh, frame))
                {
                    const int detectW = static_cast<int>(frame.width * kDetectWidthRatio);
                    feature::PanelFeatures features;
                    if (feature::MeasurePanelFeatures(frame, 0, 0, detectW, frame.height,
                                                      features))
                    {
                        const feature::PanelState state = feature::ClassifyPanel(features);
                        if (state == feature::PanelState::Closed ||
                            state == feature::PanelState::Open)
                        {
                            const bool rawOpen = (state == feature::PanelState::Open);
                            bool flippedState = false;
                            const bool flipped = debounce.Update(rawOpen, flippedState);
                            if (flipped)
                            {
                                if (rawOpen)
                                {
                                    if (suppressOpenUntilMs_.load() <= now)
                                    {
                                        if (!sentOpen && onEvent_)
                                        {
                                            onEvent_(true);
                                        }
                                        sentOpen = true;
                                        printf("[pixel] chat-open (runMed=%.2f M150=%.1f%% "
                                               "iconW240=%.2f%%)\n",
                                               features.runMedRatio, features.pctM150,
                                               features.pctIconW240);
                                        fflush(stdout);
                                    }
                                    else
                                    {
                                        printf("[pixel] open suppressed by ResetToClosed\n");
                                        fflush(stdout);
                                    }
                                }
                                else
                                {
                                    if (sentOpen && onEvent_)
                                    {
                                        onEvent_(false);
                                    }
                                    sentOpen = false;
                                    printf("[pixel] chat-close\n");
                                    fflush(stdout);
                                }
                            }
                        }
                        // NoPanel/Unknown：不动作（转场/CG 防护），保持当前状态。
                    }
                }
            }
        }
        else
        {
            // 前台非 HD2：暂停采样，去抖基态复位，避免恢复瞬间误报翻转。
            debounce.Reset(false);
        }
        // 可打断睡眠（10ms 步进，检查停止标志）。
        for (int slept = 0; slept < intervalMs_ && !stopRequested_.load(); slept += 10)
        {
            Sleep(10);
        }
    }
}
