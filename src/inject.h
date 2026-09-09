#pragma once

#include <windows.h>
#include <string>

// 读取 UTF-8 文件为宽字符串；失败或为空返回 L""。
std::wstring ReadUtf8File(const wchar_t *path);

// 向前台窗口 SendInput(KEYEVENTF_UNICODE) 注入 text —— 仅当前台匹配 HD2 时执行
// （进程名 helldivers2.exe 或标题含 HELLDIVERS），不匹配即拒绝、绝不盲发。
// submit=true 时在文本后追加一次扫描码 Enter。
// delayMs > 0：先等待（留时间切回目标窗口）再校验前台并注入。
// 返回：0=成功；3=前台不匹配被拒；4=SendInput 未写全；7=参数/文本为空。
// 复用自 hd2-input-probe（真机已验证：HD2 接受 Unicode 注入、无自动回车）。
int InjectText(const std::wstring &text, bool submit, DWORD delayMs);
