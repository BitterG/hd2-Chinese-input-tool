#include "ime.h"

#include <cstdio>
#include <vector>

namespace {

// 枚举键盘布局，按主语言挑选；preferSubLang != 0 时优先匹配子语言。
HKL FindLayoutByPrimaryLang(WORD primaryLang, WORD preferSubLang)
{
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0)
    {
        return nullptr;
    }
    std::vector<HKL> list(static_cast<size_t>(count), nullptr);
    if (GetKeyboardLayoutList(count, list.data()) <= 0)
    {
        return nullptr;
    }
    HKL fallback = nullptr;
    for (HKL hkl : list)
    {
        const WORD langid = static_cast<WORD>(reinterpret_cast<DWORD_PTR>(hkl) & 0xFFFF);
        if (PRIMARYLANGID(langid) != primaryLang)
        {
            continue;
        }
        if (preferSubLang != 0 && SUBLANGID(langid) == preferSubLang)
        {
            return hkl;
        }
        if (fallback == nullptr)
        {
            fallback = hkl;
        }
    }
    return fallback;
}

bool RequestSwitch(HWND target, HKL hkl)
{
    if (hkl == nullptr)
    {
        return false;
    }
    // 本进程线程立即生效（承载窗与主控同线程，最可靠）。
    ActivateKeyboardLayout(hkl, 0);
    // 同时向目标窗口发切换请求（跨进程尽力：游戏引擎若不处理则无效）。
    if (target != nullptr && IsWindow(target))
    {
        PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hkl));
    }
    return true;
}

const char *LayoutName(HKL hkl)
{
    static char buffer[64];
    const WORD langid = static_cast<WORD>(reinterpret_cast<DWORD_PTR>(hkl) & 0xFFFF);
    if (PRIMARYLANGID(langid) == LANG_CHINESE)
    {
        return "Chinese";
    }
    if (PRIMARYLANGID(langid) == LANG_ENGLISH)
    {
        return "English";
    }
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "langid=%04X", langid);
    return buffer;
}

} // namespace

bool SwitchToChineseInput(HWND target)
{
    HKL hkl = FindLayoutByPrimaryLang(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    if (hkl == nullptr)
    {
        printf("[ime] no Chinese keyboard layout installed — skip switch\n");
        fflush(stdout);
        return false;
    }
    printf("[ime] switch to %s (%04X)\n", LayoutName(hkl),
           static_cast<unsigned>(reinterpret_cast<DWORD_PTR>(hkl) & 0xFFFF));
    fflush(stdout);
    return RequestSwitch(target, hkl);
}

bool SwitchToEnglishInput(HWND target)
{
    HKL hkl = FindLayoutByPrimaryLang(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    if (hkl == nullptr)
    {
        printf("[ime] no English keyboard layout installed — skip switch\n");
        fflush(stdout);
        return false;
    }
    printf("[ime] switch to %s (%04X)\n", LayoutName(hkl),
           static_cast<unsigned>(reinterpret_cast<DWORD_PTR>(hkl) & 0xFFFF));
    fflush(stdout);
    return RequestSwitch(target, hkl);
}
