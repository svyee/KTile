// KTile - 轮盘插件（slugin）
//
// 与 radial_menu.h 的区别：那份是用户配置的通用轮盘，这里是插件提供、
// 只读、按进程匹配的轮盘，生命周期由目录扫描决定；两者只在渲染时都是
// RadialMenu。目录：<exe>/slugin/<id>/radial.json 清单 + icons/*.svg
// 图标（也可 png/ico），与磁贴插件 plugin/ 并列不复用。一个目录的
// processes 可覆盖多个应用，但只有一份扇区配置，想分开配置就开两个目录。
#pragma once

#include "core/radial_menu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// 轮盘插件的 schema 版本，与用户配置的 kRadialSchemaVersion 各自独立
// （插件是外部作者写的，升级节奏与主程序无关）。
inline constexpr std::uint32_t kRadialPluginSchemaVersion = 1;

struct RadialPlugin {
    std::string id;                      // 目录名，如 "vscode"
    std::string name;                    // 显示名，空则用 id
    // 条目格式 "进程名" 或 "进程名@窗口类名"（均大小写不敏感）。带类名后缀
    // 的条目要求前台窗口类也一致：explorer.exe 的文件窗口是 CabinetWClass、
    // 桌面是 Progman/WorkerW，同进程不同类，靠后缀分开配置。
    std::vector<std::string> processes;
    int priority = 0;                    // 越大越优先，解决两个插件抢同一进程
    RadialMenu menu;                     // 扇区与动作

    // 运行期字段（不来自 JSON）：插件目录绝对路径。加载时 icon 已转成
    // 绝对路径，这里留着供「打开目录」这类操作使用。
    std::string directory;

    // 加载失败时保留现场，便于设置页显示原因
    bool valid = false;
    std::string error;

    RadialPlugin() = default;
};

// 按前台进程名（及可选的窗口类名）挑一份插件轮盘：processes 命中者优先，
// priority 大者赢，仍并列则按 id 字典序取第一个（不随文件系统遍历顺序变）。
// windowClass 为空时带类名后缀的条目不命中。没有命中返回 nullptr，调用方
// 回落到用户配置的通用轮盘。
const RadialPlugin* findPluginForProcess(
    const std::vector<RadialPlugin>& plugins,
    const std::string& processName,
    const std::string& windowClass = {}) noexcept;

// 插件菜单是否可用（扇区数合法且动作数不少于扇区数）。带 when 条件的
// 变体动作允许超出扇区数，显示期过滤。插件是外部文件，校验不过会被
// 标记 valid=false 并跳过，而不是回落到默认配置（那会让用户以为插件
// 生效了）。
bool validateRadialPlugin(const RadialPlugin& plugin) noexcept;

}  // namespace core
}  // namespace ktile
