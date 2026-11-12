// KTile - Win32 平台层：开机自启动
//
// 通过注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 实现。
// 选 HKCU 而非 HKLM：无需管理员权限，也不污染全局配置。
//
#pragma once

#include "platform/win32/win32_error.h"

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

class StartupRegistration {
public:
    // 查询当前是否已注册开机自启
    static bool isEnabled(const std::wstring& valueName);

    // 启用开机自启。executablePath 为空时自动取当前进程路径。
    static Win32Status enable(const std::wstring& valueName,
                              const std::wstring& executablePath = std::wstring());

    // 禁用开机自启
    static Win32Status disable(const std::wstring& valueName);

    // 设置开关状态
    static Win32Status setEnabled(const std::wstring& valueName, bool enabled);

    // 取当前进程的可执行文件完整路径
    static Win32Status currentExecutablePath(std::wstring* outPath);
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
