// KTile - Win32 平台层：内置系统命令
//
// 「ktile:<id>」目标的执行端，每条命令都是系统级 API 的薄封装：电源类
// 用 ExitWindowsEx / LockWorkStation，媒体/音量注入多媒体虚拟键（由系统
// 与前台程序解释，比操纵混音器兼容性好），hosts 用记事本打开。Win7 SP1
// 起可用；失败只返回带可读描述的 error，弹框是 app 层的事。
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
