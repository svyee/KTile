// KTile - UI 层：「添加系统项目」选择对话框
//
// 参考自 Maye 的「添加内置项目」：分组网格，点一下即选中并返回，
// 调用方负责把 target/名称/图标写进新磁贴。
//
// 实现要点：
//   - 每个条目是一个 owner-draw 按钮：WM_DRAWITEM 里贴预乘的图标位图
//     + 画名字。不用自绘整个窗口 —— 按钮自带 Tab/回车/Esc 语义，
//     IsDialogMessageW 直接管用。
//   - 图标位图在对话框打开时统一准备（ensureBuiltinIcon 生成/复用 PNG
//     → 解码 → 预乘 → DIB）。若某项图标生成失败，格子仍然显示，
//     只是少了图标 —— 名称还在，不至于没法选。
//   - 本地模态循环与设置窗口同一机制（IsWindow 退出 + 自定义唤醒消息，
//     绝不 PostQuitMessage）。
#pragma once

#include <Windows.h>

#include <string>

namespace ktile {
namespace ui {

struct BuiltinPickResult {
    std::wstring id;      // 内置项标识（"lock"）
    std::wstring name;    // 显示名（"锁屏"）
    std::wstring target;  // item.target 应取的值（"ktile:lock" 或 shell 路径）
    std::wstring icon;    // item.icon 应取的存储值（"res\builtin-lock.png"）
};

// 弹出选择网格。用户点击任一条目返回 true 并填充 out；取消/关闭返回 false。
bool showBuiltinItemDialog(HWND owner, BuiltinPickResult* out);

}  // namespace ui
}  // namespace ktile
