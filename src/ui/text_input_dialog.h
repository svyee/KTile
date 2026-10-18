// KTile - UI 层：单行文本输入对话框
//
// 用途：只需要用户给一个名字的场合 —— 目前是「新建标签页」与
// 「重命名标签页」。
//
// 为什么不复用项目编辑对话框：那个对话框有一整屏控件（图标预览、
// 磁贴尺寸、类型提示、目标浏览……），为了一次改名把它全部搭起来，
// 用户看到的是一个「编辑项目」的大窗口，标题却是「重命名」——
// 视觉上就在撒谎。这里只要一个标签 + 一个输入框 + 两个按钮。
//
// 不带 .rc 资源，控件全部代码生成（与项目编辑/设置窗口同一套路）。
//
#pragma once

#include <Windows.h>

#include <string>

namespace ktile {
namespace ui {

// 弹出文本输入对话框（本地模态）。
//
// owner      : 拥有者窗口（决定居中位置与点击判定）
// title      : 标题栏文字
// prompt     : 输入框上方的说明文字
// initialText: 初始内容（新建时可为空）
// outText    : 用户点「确定」时写入裁掉首尾空白后的结果
//
// 返回 true 表示确认（此时 *outText 一定非空 —— 空输入会被挡在对话框里）；
// 返回 false 表示取消（含直接关窗）。
//
// 需要 DPI 缩放：字体与控件尺寸按 owner 的 DPI 缩放，高 DPI 下不糊。
bool showTextInputDialog(HWND owner,
                         const std::wstring& title,
                         const std::wstring& prompt,
                         const std::wstring& initialText,
                         std::wstring* outText);

}  // namespace ui
}  // namespace ktile
