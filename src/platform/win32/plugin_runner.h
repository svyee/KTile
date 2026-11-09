#pragma once

// 插件数据源：外部命令行程序
//
// 生命周期约定：每次刷新只执行一次，进程结束即退出；不驻留、不常驻后台。
// 输出约定：stdout 为 UTF-8 JSON（或 manifest 指定的 outputFile）。
// 参数约定：磁贴参数以 JSON 形式放进环境变量 KTILE_PLUGIN_CONFIG。
//
// 安全/健壮性：
//   - CREATE_NO_WINDOW，避免闪黑框
//   - Job Object + KILL_ON_JOB_CLOSE，超时/退出时连同子进程一起杀
//   - 默认 8 秒超时，防止脚本卡死拖住刷新线程

#include <string>
#include <vector>

#include "platform/win32/plugin_providers.h"

namespace ktile {
namespace platform {
namespace win32 {

PluginRawResult runCommandPlugin(
    const std::string& program,
    const std::vector<std::string>& arguments,
    const std::string& workingDirectory,
    const std::string& configJson,
    const std::string& outputFile,
    int timeoutSeconds);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
