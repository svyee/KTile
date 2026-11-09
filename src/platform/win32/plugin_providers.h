#pragma once

// 插件数据源：内置本地信息
//
// builtin 源完全在进程内完成（CPU/内存/磁盘/电池/时间），不创建进程、
// 不发网络请求，刷新开销可以忽略。返回值统一是 UTF-8 JSON 文本，
// 与 http / command 走同一条解析路径。

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

struct PluginRawResult {
    bool ok = false;
    std::string body;   // UTF-8 JSON
    std::string error;

    PluginRawResult() = default;
};

// provider 名称（v1 只有 "system"）
PluginRawResult runBuiltinPluginProvider(const std::string& provider);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
