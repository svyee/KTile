#pragma once

// 插件数据源：HTTP(S)
//
// 使用 WinHTTP（Win7 自带），支持 GET/POST、自定义头与请求体。
// GraphQL 只是「POST + JSON body」，不需要特殊分支。
//
// 刻意不做 OAuth 刷新流：那类鉴权需要持久化令牌、定时续期、失败重试，
// 复杂度与「插件只在面板唤出时跑一次」的定位冲突。需要登录态的接口
// 更适合用 command 源调用官方 CLI。

#include <string>
#include <utility>
#include <vector>

#include "platform/win32/plugin_providers.h"

namespace ktile {
namespace platform {
namespace win32 {

PluginRawResult httpRequestPlugin(
    const std::string& url,
    const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    int timeoutSeconds);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
