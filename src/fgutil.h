#pragma once

#include <windows.h>
#include <string>

// 前台窗口的进程可执行文件名（小写）。成功返回 true 并回填 pid。
bool ForegroundProcessName(std::wstring &name, DWORD &pid);

// 前台是否匹配 HD2 关键词：进程名 helldivers2.exe，或标题含 HELLDIVERS。
// outHwnd / outTitle 为可选输出。复用自 hd2-input-probe。
bool IsForegroundHd2Like(HWND *outHwnd, std::wstring *outTitle);

// 打印前台窗口完整诊断（fg 子命令）。返回 0 表示成功。
int PrintForegroundDiagnostics();
