#pragma once

#include <windows.h>

// 输入法（键盘布局）自动切换：呼出打字态时转中文、退出时转回英文。
// 做法：枚举系统已装键盘布局，按语言挑选（中文优先简体、英文优先美式），
// 切换时既对本进程线程 ActivateKeyboardLayout（承载窗同线程，最直接生效），
// 也向目标窗口 PostMessage(WM_INPUTLANGCHANGEREQUEST)（对游戏窗口尽力而为——
// 引擎若不处理该消息则无效）。返回是否成功发起切换。
bool SwitchToChineseInput(HWND target);
bool SwitchToEnglishInput(HWND target);
