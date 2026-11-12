// KTile - Win32 平台层：UUID 生成
//
// 磁贴的 item.id 是跨会话的稳定标识（图标缓存键、启动统计、右键菜单定位），
// 须全局唯一且删掉中间一块后不重用；自增数字会与手改配置、外部导入冲突，
// 故用随机 GUID，代价只是 id 长一点（36 字节）。
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 生成一个 UUID（形如 "3f2504e0-4f89-41d3-9a0c-0305e82c3301"，小写无花括号）。
// 失败（极少见，系统 API 出错）时返回空串，由调用方决定如何回退。
std::string createGuidString();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
