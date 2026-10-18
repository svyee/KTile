// KTile - 设置窗口
//
// 目前承载「网格行列编辑器」+ 两项常用开关。
//
// 设计取舍：
//
// 1. 自建窗口而不是资源脚本对话框。
//    本项目刻意不带 .rc 资源（全量代码生成 UI），理由见 tile_context_menu.cpp：
//    避免 rc.exe 依赖，也便于 AI 协作修改 —— 资源脚本的二进制格式对
//    文本编辑不友好。
//
// 2. 走「模态对话框」而不是「独立设置窗口」。
//    网格尺寸改动会立刻重建布局，若做成非模态窗口，用户可能同时拖拽磁贴，
//    产生「布局正在重建 + 拖拽状态机运行中」的交叠。模态是最省心的隔离。
//
// 3. 结果通过 SettingsResult 一次性返回，不在这里直接改配置。
//    窗口层不认识 ApplicationConfig 的持久化细节；由 Application 决定
//    哪些字段要落盘、哪些要立刻应用到窗口。
//
#pragma once

#include <Windows.h>

#include "core/data_model.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ktile {
namespace ui {

// 设置窗口的编辑结果。
//
// 用 optional 表达「该项未被改动」：调用方可以先比价再决定是否落盘，
// 避免无意义的写文件与重建布局。
struct SettingsResult {
    // 网格列数（3..16）
    std::optional<std::uint16_t> gridColumns;
    // 网格行数（3..16）
    std::optional<std::uint16_t> gridRows;
    // 是否显示磁贴文字标签
    std::optional<bool> showTileLabels;
    // 标题是否默认显示（false = 仅鼠标移入时显示）
    std::optional<bool> alwaysShowTileLabels;
    // 标签栏是否放在窗口底部（false = 顶部，默认）
    std::optional<bool> tabBarAtBottom;
    // 窗口出现位置
    std::optional<core::WindowPlacement> windowPlacement;
    // 皮肤显示名（<data>\themes\<名称>.json）。未改动则留空
    std::optional<std::string> themeName;
    // 背景图：非空 = 新选的图片绝对路径（由调用方复制进 themes\backgrounds）；
    // 空串 = 用户点了「清除」。未改动则留空
    std::optional<std::string> backgroundImage;
    // 是否开机自动启动
    std::optional<bool> startWithWindows;
    // 唤醒快捷键（预设）。未改动则留空
    std::optional<core::HotkeyPreset> hotkeyPreset;
    // 主窗口整体不透明度（50~100）
    std::optional<int> windowOpacityPercent;
    // 磁贴背景不透明度（0~100；-1 = 跟随皮肤）
    std::optional<int> tileOpacityPercent;
};

// 实时预览回调（可选）。
//
// 主窗口在设置窗口旁边保持可见，这几项「一改就想看到」的设置都会即时
// 反映到主窗口，但**都不落盘**：点「确定」才写 config，点「取消」由
// 调用方重新套用配置（皮肤/背景图/标签栏位置各一次）即可还原。
struct SettingsPreviewHooks {
    // 参数是下拉里选中的皮肤显示名（UTF-8）
    std::function<void(const std::string& themeName)> themeChanged;
    // 标签栏位置（true = 窗口底部）
    std::function<void(bool atBottom)> tabBarAtBottomChanged;
    // 背景图：参数是图片的绝对路径；空串 = 清除。
    // 注意传的是**原文件**路径而不是导入后的存储路径 —— 预览阶段不该
    // 往 data\themes\backgrounds 里写文件，否则点「取消」会留下垃圾。
    std::function<void(const std::wstring& absolutePath)> backgroundChanged;
    // 网格尺寸（列数/行数任一变化就回调，值已夹到合法范围）
    std::function<void(std::uint16_t columns, std::uint16_t rows)> gridChanged;
    // 标签总闸（显示/不显示磁贴文字标签）
    std::function<void(bool showLabels)> showTileLabelsChanged;
    // 标题默认行为（true = 一直显示）
    std::function<void(bool alwaysShow)> alwaysShowTitleChanged;
    // 主窗口整体不透明度（50~100）。拖动滑块过程中就会连续回调。
    std::function<void(int percent)> windowOpacityChanged;
    // 磁贴背景不透明度（0~100）。-1 表示「跟随皮肤」。
    std::function<void(int percent)> tileOpacityChanged;
};

// 显示设置窗口（模态）。用户点「确定」返回 true 并填充 out；
// 点「取消」/ 关闭返回 false，out 不被修改。
//
// 参数：
//   owner                父窗口（用于居中与模态归属，可为 nullptr）
//   currentColumns       当前网格列数（3..16）
//   currentRows          当前网格行数（3..16）
//   currentShowLabels    当前是否显示磁贴标签
//   currentAlwaysShowTitle 当前标题是否默认显示
//   currentTabBarAtBottom 当前标签栏是否在窗口底部
//   currentWindowPlacement 当前窗口出现位置
//   currentThemeName     当前皮肤显示名（UTF-8，可为空 = 默认皮肤）
//   currentBackground    当前背景图存储值（UTF-8，空 = 未设置；仅用于显示）
//   currentStartup       当前是否开机自启
//   currentHotkeyPreset  当前唤醒快捷键预设
//   currentWindowOpacity 当前主窗口整体不透明度（50~100）
//   currentTileOpacity   当前磁贴背景不透明度（0~100；-1 = 跟随皮肤）
//   columnsInUse         当前布局已占用的最大列数（用于提示「缩小会挤掉磁贴」）
//   rowsInUse            当前布局已占用的最大行数
//   out                  结果输出
//   preview              实时预览回调（可为 nullptr）
bool showSettingsWindow(HWND owner,
                        std::uint16_t currentColumns,
                        std::uint16_t currentRows,
                        bool currentShowLabels,
                        bool currentAlwaysShowTitle,
                        bool currentTabBarAtBottom,
                        core::WindowPlacement currentWindowPlacement,
                        const std::string& currentThemeName,
                        const std::string& currentBackground,
                        bool currentStartup,
                        core::HotkeyPreset currentHotkeyPreset,
                        int currentWindowOpacity,
                        int currentTileOpacity,
                        std::uint16_t columnsInUse,
                        std::uint16_t rowsInUse,
                        SettingsResult* out,
                        const SettingsPreviewHooks* preview = nullptr);

}  // namespace ui
}  // namespace ktile
