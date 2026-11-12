// KTile - Win32 平台层：以 exe 所在目录为基准的路径解析
//
// 绿色软件要求整个目录拷走就能用，磁贴里的路径须能相对 exe 目录书写，
// 不能依赖当前工作目录（由启动方式决定）。只认带目录分隔符的相对写法；
// 裸名字与 URL 原样放行，交给系统处理。
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// KTile.exe 所在目录（不带结尾反斜杠）。取不到时返回空串。
std::wstring executableDirectory();

// 是否是「指向 exe 目录内（或附近）的相对路径」。判据刻意窄：必须含
// 目录分隔符、非绝对路径、非 URL，"notepad.exe" 与 "https://..." 不会误判。
bool isProgramRelativePath(const std::wstring& path);

// 把相对 exe 目录的路径解析成绝对路径。绝对路径、URL、裸命令名原样返回
// （只做环境变量展开与空白裁剪）；相对路径拼到 exe 目录后做 .. / . 规范化。
// 结果不要求文件存在：启动失败时报出绝对路径比报「找不到 .\x.exe」有用。
std::wstring resolveProgramRelativePath(const std::wstring& path);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
