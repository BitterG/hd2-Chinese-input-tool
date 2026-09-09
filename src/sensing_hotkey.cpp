#include "sensing.h"

#include <cstdio>
#include <utility>

void HotkeySensingSource::Start(std::function<void(bool chatOpen)> onEvent)
{
    onEvent_ = std::move(onEvent);
    chatOpen_ = false;
    if (RegisterHotKey(nullptr, kHotkeyId, MOD_NOREPEAT, kToggleVk))
    {
        running_ = true;
        printf("[sensing-hotkey] registered F8 (id=%d) — press to toggle chat-open/close\n",
               kHotkeyId);
        fflush(stdout);
    }
    else
    {
        printf("[sensing-hotkey] RegisterHotKey failed lastError=%lu\n", GetLastError());
    }
}

bool HotkeySensingSource::HandleHotkey(WPARAM hotkeyId)
{
    if (!running_ || static_cast<int>(hotkeyId) != kHotkeyId)
    {
        return false;
    }
    chatOpen_ = !chatOpen_;
    printf("[sensing-hotkey] toggle -> chat-open=%d\n", chatOpen_ ? 1 : 0);
    fflush(stdout);
    if (onEvent_)
    {
        onEvent_(chatOpen_);
    }
    return true;
}

void HotkeySensingSource::Stop()
{
    if (running_)
    {
        UnregisterHotKey(nullptr, kHotkeyId);
        running_ = false;
        printf("[sensing-hotkey] stopped\n");
        fflush(stdout);
    }
}
