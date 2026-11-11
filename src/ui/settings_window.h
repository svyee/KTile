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
#include "core/radial_menu.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
    // 界面语言代码（"auto" = 跟随系统）。未改动则留空。
    //
    // 与其它字段一样走 optional：应用层据此判断「是否要落盘 + 是否要
    // 重新取词」，避免每次确定都把语言重设一遍（会丢掉界面上的临时状态）。
    std::optional<std::string> language;
    // 背景图：非空 = 新选的图片绝对路径（由调用方复制进 themes\backgrounds）；
    // 空串 = 用户点了「清除」。未改动则留空
    std::optional<std::string> backgroundImage;
    // 是否开机自动启动
    std::optional<bool> startWithWindows;
    // 跟随启动清单（KTile 启动时一并拉起的程序）。
    //
    // 整份交回而不是逐条增删：界面上的「添加 / 移除」都是对一张表的编辑，
    // 交回最终结果最简单，也不用为「删掉了哪一条」设计一套事件。
    std::optional<std::vector<core::FollowLaunchEntry>> followLaunch;
    // 唤醒快捷键（预设）。未改动则留空
    std::optional<core::HotkeyPreset> hotkeyPreset;
    // 主窗口整体不透明度（50~100）
    std::optional<int> windowOpacityPercent;
    // 磁贴背景不透明度（0~100；-1 = 跟随皮肤）
    std::optional<int> tileOpacityPercent;
    // 全局默认磁贴底色。nullopt = 未改动；有值但内层为空 = 恢复跟随皮肤；
    // 有值且内层非空 = 新的默认色（ARGB）。
    std::optional<std::optional<std::uint32_t>> defaultTileColor;
    // 图标相对磁贴内框的统一缩放（50~100）
    std::optional<int> iconScalePercent;

    // 轮盘菜单整份配置（触发方式 + 抑制键 + 通用扇区 + 进程黑名单）。
    //
    // 为什么整份交回而不是拆成几个 optional 字段：扇区动作是一个数组，
    // 拆字段既表达不了「清空某个扇区」，也会让 SettingsResult 膨胀成
    // 一堆半成品。调用方拿到后整份替换并落盘即可。
    std::optional<core::RadialConfig> radial;
};

// 插件管理页的一行（只读展示）。
//
// UI 层不认识 PluginController，也不解析 plugin.json：应用层把清单里的
// 字段翻译成这份「显示用」数据，设置窗口只负责画列表与转发用户动作。
struct SettingsPluginInfo {
    std::string id;
    std::string name;
    // 数据源类型的中文标签（本地 / 网络 / 命令行）
    std::string sourceLabel;
    // 刷新间隔（秒）。<= 0 表示未设置。
    int refreshSeconds = 0;
    // 面板可见时的高频刷新间隔（秒）。<= 0 表示只在唤出时刷新。
    int visibleRefreshSeconds = 0;
};

// 插件管理页的宿主回调（可选）。
//
// 安装 / 删除会直接改动磁盘上的插件目录，所以这些动作用回调交给应用层，
// 而不是在 UI 层直接操作文件 —— 与「设置窗口只收集结果」的整体约定一致。
struct SettingsPluginHooks {
    // 拉取当前已发现的插件列表（每次切到插件页 / 操作成功后都会调用）
    std::function<std::vector<SettingsPluginInfo>()> list;
    // 安装：参数是用户选择的 plugin.json 绝对路径。
    std::function<bool(const std::wstring& manifestPath, std::string* error)> install;
    // 删除：参数是插件 id。
    std::function<bool(const std::string& id, std::string* error)> remove;
    // 在资源管理器中打开插件目录（id 为空表示插件根目录）。
    std::function<void(const std::string& id)> openFolder;
    // 把插件添加成当前标签页的一块磁贴。
    std::function<void(const std::string& id)> addTile;
};

// 轮盘插件（slugin）页的一行（只读展示）。
//
// 与上面的 SettingsPluginInfo 分开：轮盘插件的清单格式、匹配方式、失败原因
// 全都不同，混用一个结构会让两个页面互相迁就字段。
struct SettingsRadialPluginInfo {
    std::string id;
    std::string name;
    // 已拼好的进程列表显示串（"explorer.exe, Code.exe"）
    std::string processes;
    int sectorCount = 0;
    int actionCount = 0;
    // 加载成功与否；失败时 error 里是原因
    bool valid = true;
    std::string error;
};

// 轮盘插件页的宿主回调（可选）。
//
// 这一页**只读**：轮盘插件的形态是「一个目录 + 一份 JSON + 一堆图标」，
// 用表格去编辑扇区与快捷键会把插件退化成「只能在界面里配」的东西，
// 也就丢掉了「加个目录就是加个应用」的全部好处。所以界面只负责
// 「让你知道加载了哪些、为什么某个没加载、一键打开那份 JSON」。
struct SettingsRadialPluginHooks {
    // 拉取当前已发现的轮盘插件（每次切到该页 / 重新扫描后调用）
    std::function<std::vector<SettingsRadialPluginInfo>()> list;
    // 用系统默认编辑器打开某个插件的 radial.json（记事本兜底）
    std::function<void(const std::string& id)> openManifest;
    // 在资源管理器里打开插件目录（id 为空表示 slugin 根目录）
    std::function<void(const std::string& id)> openFolder;
    // 重新扫描磁盘（回调返回后界面会重新调用 list）
    std::function<void()> rescan;
    // 插件根目录（用于页面上的说明文字）
    std::function<std::wstring()> rootDirectory;
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
    // 全局默认磁贴底色（空 = 跟随皮肤）。
    std::function<void(std::optional<std::uint32_t> color)> defaultTileColorChanged;
    // 图标统一缩放（50~100）。拖动滑块过程中就会连续回调。
    std::function<void(int percent)> iconScaleChanged;
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
//   currentStartup       当前是否开机自启
//   currentFollowLaunch  当前的跟随启动清单
//   currentHotkeyPreset  当前唤醒快捷键预设
//   currentWindowOpacity 当前主窗口整体不透明度（50~100）
//   currentTileOpacity   当前磁贴背景不透明度（0~100；-1 = 跟随皮肤）
//   currentIconScale     当前图标统一缩放（50~100）
//   currentDefaultTileColor 当前全局默认磁贴底色（可空 = 跟随皮肤）
//   currentRadial        当前轮盘菜单配置（radial.json 的内存副本）
//   columnsInUse         当前布局已占用的最大列数（用于提示「缩小会挤掉磁贴」）
//   rowsInUse            当前布局已占用的最大行数
//   out                  结果输出
//   preview              实时预览回调（可为 nullptr）
//   plugins              插件管理页回调（可为 nullptr；为空时安装/删除按钮隐藏）
//   radialPlugins        轮盘插件页回调（可为 nullptr；为空时该页只有说明文字）
bool showSettingsWindow(HWND owner,
                        std::uint16_t currentColumns,
                        std::uint16_t currentRows,
                        bool currentShowLabels,
                        bool currentAlwaysShowTitle,
                        bool currentTabBarAtBottom,
                        core::WindowPlacement currentWindowPlacement,
                        const std::string& currentThemeName,
                        const std::string& currentLanguage,
                        bool currentStartup,
                        const std::vector<core::FollowLaunchEntry>&
                            currentFollowLaunch,
                        core::HotkeyPreset currentHotkeyPreset,
                        int currentWindowOpacity,
                        int currentTileOpacity,
                        int currentIconScale,
                        const std::optional<std::uint32_t>& currentDefaultTileColor,
                        const core::RadialConfig& currentRadial,
                        std::uint16_t columnsInUse,
                        std::uint16_t rowsInUse,
                        SettingsResult* out,
                        const SettingsPreviewHooks* preview = nullptr,
                        const SettingsPluginHooks* plugins = nullptr,
                        const SettingsRadialPluginHooks* radialPlugins = nullptr);

}  // namespace ui
}  // namespace ktile
