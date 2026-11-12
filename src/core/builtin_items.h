// KTile - 内置项目表：「添加项目 → 系统项…」对话框里的系统功能磁贴。
//
// 功能命令（电源/媒体/音量/hosts）target 为 "ktile:<id>"，启动时由
// item_launcher 分派到 system_actions；系统位置（此电脑/回收站等）target
// 直接存 shell 路径或 exe 名，走 ShellExecuteW 通路。图标由 iconSource
// 决定：nullptr 用 Lucide 线框 + 蓝圆角底，非 nullptr 从真实 shell 对象
// 提取系统图标，比统一线框更好认。不做「禁止/允许息屏」这类状态开关
// （交互语义不同）；睡眠/休眠已移除，休眠常被 powercfg 禁用，点了没反应
// 会被当成程序坏了。
#pragma once

#include <string>
#include <vector>

namespace ktile {
namespace core {

struct BuiltinItem {
    const wchar_t* id;        // 短标识（"lock"）：target、图标、菜单都用它
    const wchar_t* name;      // 显示名（"锁屏"）
    const wchar_t* category;  // 分组标题（"电源" / "媒体与音量" / "系统位置"）
    const wchar_t* target;    // 写进 item.target 的值
    // 图标来源（文件路径 / "::{CLSID}" / "shell:startup"）。nullptr = 矢量图标。
    const wchar_t* iconSource;
};

// 三组的展示顺序也在这里定：使用频率优先（锁屏/静音最常用）
inline const std::vector<BuiltinItem>& builtinItems() {
    static const std::vector<BuiltinItem> items = {
        // ---- 电源 ----
        {L"lock",     L"锁屏", L"电源", L"ktile:lock",     nullptr},
        {L"logoff",   L"注销", L"电源", L"ktile:logoff",   nullptr},
        {L"restart",  L"重启", L"电源", L"ktile:restart",  nullptr},
        {L"shutdown", L"关机", L"电源", L"ktile:shutdown", nullptr},

        // ---- 媒体与音量 ----
        {L"volume-up",       L"音量+",   L"媒体与音量", L"ktile:volume-up",
         nullptr},
        {L"volume-down",     L"音量-",   L"媒体与音量", L"ktile:volume-down",
         nullptr},
        {L"volume-mute",     L"静音",    L"媒体与音量", L"ktile:volume-mute",
         nullptr},
        {L"media-prev",      L"上一首",  L"媒体与音量", L"ktile:media-prev",
         nullptr},
        {L"media-playpause", L"播放暂停", L"媒体与音量",
         L"ktile:media-playpause", nullptr},
        {L"media-next",      L"下一首",  L"媒体与音量", L"ktile:media-next",
         nullptr},

        // ---- 系统位置 ----
        // target 为 shell 路径 / exe 名，ShellExecuteW 直接认识；
        // iconSource 与 target 相同。
        {L"this-pc",      L"此电脑",     L"系统位置",
         L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
         L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"},
        {L"recycle-bin",  L"回收站",     L"系统位置",
         L"::{645FF040-5081-101B-9F08-00AA002F954E}",
         L"::{645FF040-5081-101B-9F08-00AA002F954E}"},
        {L"control-panel", L"控制面板",  L"系统位置", L"control.exe",
         L"control.exe"},
        {L"network",      L"网络",       L"系统位置",
         L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}",
         L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"},
        {L"printers",     L"打印机",     L"系统位置",
         L"::{2227A280-3AEA-1069-A2DE-08002B30309D}",
         L"::{2227A280-3AEA-1069-A2DE-08002B30309D}"},
        {L"startup-folder", L"启动目录", L"系统位置", L"shell:startup",
         L"shell:startup"},
        {L"cmd",          L"命令提示符", L"系统位置", L"cmd.exe", L"cmd.exe"},
        {L"calc",         L"计算器",     L"系统位置", L"calc.exe", L"calc.exe"},
        {L"notepad",      L"记事本",     L"系统位置", L"notepad.exe",
         L"notepad.exe"},
        {L"mspaint",      L"画板",       L"系统位置", L"mspaint.exe",
         L"mspaint.exe"},
        // hosts 是功能命令而非路径：无扩展名文件直接启动会弹「选择打开方式」。
        // 图标不用记事本，避免与上面的磁贴撞脸，保留 Lucide 文档图标。
        {L"hosts",        L"Hosts",      L"系统位置", L"ktile:hosts",
         nullptr},
    };
    return items;
}

// target 是否是内置功能命令（"ktile:<id>" 形式）。
// 系统位置项返回空，它们走普通启动通路。
inline std::wstring builtinIdFromTarget(const std::wstring& target) {
    const std::wstring prefix = L"ktile:";
    if (target.size() <= prefix.size() ||
        target.compare(0, prefix.size(), prefix) != 0) {
        return std::wstring();
    }
    return target.substr(prefix.size());
}

inline const BuiltinItem* findBuiltinItem(const std::wstring& id) {
    if (id.empty()) {
        return nullptr;
    }
    for (const BuiltinItem& item : builtinItems()) {
        if (id == item.id) {
            return &item;
        }
    }
    return nullptr;
}

// 内置项图标的存储文件名（固定按 id，重复添加复用同一份 PNG）
inline std::wstring builtinIconFileName(const std::wstring& id) {
    return L"res\\builtin-" + id + L".png";
}

}  // namespace core
}  // namespace ktile
