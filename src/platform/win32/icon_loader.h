// KTile - Win32 平台层：图标提取
//
// 从启动目标提取高质量图标供磁贴渲染，优先级：item.icon 指定的图标文件
//（位图 png/jpg/bmp/gif/tif/ico 走 WIC 解码，exe/dll/cpl 走 shell 提取；
// 相对 data 目录的路径解析成绝对路径，绝对路径原样兼容老数据）→ target
// 可执行文件（PE 用 PrivateExtractIconsW 按请求尺寸直接提取，exe 之外的
// 大图标走 ImageList jumbo 档，带大画布装小图退化检测）→ 文件关联图标
//（SHGetFileInfo，适合文档/文件夹/URL）→ 全部失败返回空，调用方用默认
// 色块兜底。优先请求 256x256（jumbo）源图，降采样质量最好。
//
#pragma once

#include "platform/win32/unique_handles.h"

#include <Windows.h>

#include <cstdint>
#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 图标请求尺寸档位
enum class IconSizeHint : std::uint8_t {
    Small = 0,   // 32px（缓存友好）
    Medium = 1,  // 64px（小磁贴用）
    Large = 2,   // 256px jumbo（大磁贴用，渲染时降采样）
};

// 按启动项类型与目标提取图标。
//
// targetUtf8  : 可执行文件 / 文档 / 文件夹的路径，或 URL
// customIconUtf8 : 自定义图标文件路径（可为空字符串）
// isFolder    : 目标是否为文件夹（决定是否用 SHGFI 的文件夹图标）
// isUrl       : 目标是否为 URL（用默认浏览器图标）
//
// 返回拥有所有权的 HICON；失败返回空句柄（value.valid() == false）。
platform::win32::UniqueHIcon loadItemIcon(const std::string& targetUtf8,
                                          const std::string& customIconUtf8,
                                          bool isFolder,
                                          bool isUrl,
                                          IconSizeHint hint);

// 从指定文件按大图标提取（内部使用）
platform::win32::UniqueHIcon loadIconFromFile(const std::wstring& filePath,
                                              IconSizeHint hint);

// 取「默认应用程序」通用图标（所有提取都失败时的兜底）
platform::win32::UniqueHIcon loadDefaultApplicationIcon(IconSizeHint hint);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
