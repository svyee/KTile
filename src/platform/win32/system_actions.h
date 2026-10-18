// KTile - Win32 平台层：内置系统命令
//
// 「ktile:<id>」目标的执行端。每条命令都是系统级 API 的薄封装：
//   - 电源类：ExitWindowsEx / SetSuspendState / LockWorkStation
//   - 显示器：WM_SYSCOMMAND + SC_MONITORPOWER（广播，历史遗留但仍是
//     Win7~Win11 都认的正规通路）
//   - 媒体/音量：注入标准多媒体虚拟键，让系统（和正在播放的程序）自己
//     处理 —— 比直接操纵混音器兼容性好得多，Win7~Win11 行为一致
//   - hosts：用记事本打开系统 hosts 文件
//
// 设计约束：Win7 SP1 起可用；失败返回带可读描述的 error，不弹框 ——
// 弹框是 app 层的事，平台层只报告。
#pragma once

#include "platform/win32/item_launcher.h"

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 执行内置命令。id 是 "ktile:" 后面的部分（如 "lock"）。
// 未知 id 返回失败（表里有的才会进到这一步，防御手改配置）。
LaunchResult runBuiltinAction(const std::wstring& id);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
