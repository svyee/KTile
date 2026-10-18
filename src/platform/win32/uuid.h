// KTile - Win32 平台层：UUID 生成
//
// 磁贴的 item.id 是跨会话的稳定标识（图标缓存键、启动统计、右键菜单
// 定位都靠它），必须全局唯一且不会因为「删掉中间一块」而重用。
//
// 为什么不用「当前最大 id + 1」这种朴素做法：
//   1. 用户手工编辑过 items.json 后，数字不一定是连续的；
//   2. 从别人那里拷一份磁贴过来，id 很可能与本地撞车；
//   3. 将来要支持导入/导出，自增数字必然冲突。
// 随机 GUID 一次解决这三件事，代价只是 id 长一点（36 字节，可忽略）。
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
