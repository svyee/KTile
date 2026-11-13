// KTile - 设置窗口（模态，自建窗口，不用 .rc 资源脚本对话框）。
//
// 网格改动会立刻重建布局，模态可隔离正在进行的磁贴拖拽；结果通过
// SettingsResult 一次性返回，不直接改配置，由 Application 决定落盘。
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

// 设置窗口的编辑结果。optional = 该项未改动，调用方据此跳过落盘与重建。
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
    // 界面语言代码（"auto" = 跟随系统）。未改动则留空；应用层据此决定
    // 是否重新取词，避免每次确定都重设一遍（会丢界面上的临时状态）。
    std::optional<std::string> language;
    // 背景图：非空 = 新选的图片绝对路径（调用方复制进 themes\backgrounds）；
    // 空串 = 用户点了「清除」。未改动则留空
    std::optional<std::string> backgroundImage;
    // 是否开机自动启动
    std::optional<bool> startWithWindows;
    // 窗口置顶快捷键（enabled=false 表示关闭该功能）
    std::optional<core::HotkeyConfig> topmostHotkey;
    // 跟随启动清单。整份交回：界面上是对一张表的编辑，不为逐条增删设计事件。
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
    // 整份交回：扇区动作是数组，拆 optional 字段表达不了「清空某个扇区」，
    // 调用方拿到后整份替换并落盘即可。
    std::optional<core::RadialConfig> radial;
};

// 插件管理页的一行（只读展示）。UI 不认识 PluginController 也不解析
// plugin.json：应用层把清单翻译成显示数据，窗口只画列表与转发动作。
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

// 插件管理页的宿主回调（可选）。安装 / 删除直接改磁盘上的插件目录，
// 动作交给应用层，与「设置窗口只收集结果」的约定一致。
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

// 轮盘插件（slugin）页的一行（只读展示）。与 SettingsPluginInfo 分开：
// 轮盘插件的清单格式与匹配方式完全不同，混用一个结构会让两页互相迁就。
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

// 轮盘插件页的宿主回调（可选）。该页只读：轮盘插件是「一个目录 + 一份
// JSON + 一堆图标」，表格编辑会退化成只能在界面里配；界面只负责展示
// 加载情况、跳过原因，以及一键打开那份 JSON。
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

// 「附加小工具」页录制快捷键时的宿主回调（可选）。录制要拿到原始按键，
// 而被 RegisterHotKey 注册中的组合只会变成 WM_HOTKEY 送给消息窗口，对话框
// 一个按键也收不到——录 F7 没反应就是这个原因，所以录制期间必须临时注销。
struct SettingsHotkeyCaptureHooks {
    // true = 开始录制（注销全局热键），false = 结束（按已生效配置重新注册）
    std::function<void(bool capturing)> captureChanged;
};

// 实时预览回调（可选）。这几项一改就反映到旁边保持可见的主窗口，但都
// 不落盘：点「确定」才写 config，点「取消」由调用方重新套用配置还原。
struct SettingsPreviewHooks {
    // 参数是下拉里选中的皮肤显示名（UTF-8）
    std::function<void(const std::string& themeName)> themeChanged;
    // 标签栏位置（true = 窗口底部）
    std::function<void(bool atBottom)> tabBarAtBottomChanged;
    // 背景图：参数是图片绝对路径；空串 = 清除。传原文件路径而非导入后的
    // 存储路径：预览阶段不往 data\themes\backgrounds 写文件，否则取消会留垃圾。
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

// 显示设置窗口（模态）。点「确定」返回 true 并填充 out；点「取消」或关闭
// 返回 false，out 不被修改。
// current* 参数是各项设置的当前值；columnsInUse / rowsInUse 是布局已占用的
// 最大行列数（用于「缩小会挤掉磁贴」的提示）；preview / plugins /
// radialPlugins 为可选回调，为空时对应功能隐藏。
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
                        const core::HotkeyConfig& currentTopmostHotkey,
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
                        const SettingsRadialPluginHooks* radialPlugins = nullptr,
                        const SettingsHotkeyCaptureHooks* hotkeyCapture = nullptr);

}  // namespace ui
}  // namespace ktile
