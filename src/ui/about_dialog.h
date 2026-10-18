// KTile - UI 层：「关于」对话框
//
// 用途：展示产品名（KTile 磁贴快启）、版本号、联系方式。
// 入口：托盘菜单「关于 KTile...」与主窗口空白区右键菜单同名项。
//
// 与 text_input_dialog 同一套骨架：代码生成控件（不带 .rc 资源）、
// 本地模态循环（IsWindow 退出 + kDialogCloseMessage 唤醒，
// 绝不 PostQuitMessage）、控件尺寸按 owner 的 DPI 缩放。
//
#pragma once

#include <Windows.h>

#include <string>

namespace ktile {
namespace ui {

// 弹出「关于」对话框（本地模态，打开期间托盘与热键照常工作）。
//
// owner     : 拥有者窗口（决定居中位置；可为 nullptr，居中于屏幕）
// productName: 产品名（如 L"KTile 磁贴快启"）
// version   : 版本号（如 L"0.9.0"）
void showAboutDialog(HWND owner,
                     const std::wstring& productName,
                     const std::wstring& version);

}  // namespace ui
}  // namespace ktile
