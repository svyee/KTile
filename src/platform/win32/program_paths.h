// KTile - Win32 平台层：以 KTile.exe 所在目录为基准的路径解析
//
// 为什么要这一层：绿色软件的卖点是「整个目录拷走就能用」。可只要磁贴里
// 存的是 C:\Users\me\KTile\tools\x.exe 这种绝对路径，换个位置（换机器、
// 换盘符、换个文件夹）就全断了。允许写相对路径之后，磁贴跟着目录走。
//
// 基准是 **exe 所在目录**，不是当前工作目录：后者由用户从哪儿启动、
// 是否从快捷方式启动决定，绿色软件不能依赖它。
//
// 只认「看起来像路径」的相对写法（带目录分隔符），裸名字放行交给系统：
//   .\tools\app.exe       → <exe目录>\tools\app.exe
//   tools\app.exe         → <exe目录>\tools\app.exe
//   ..\bin\app.exe        → <exe目录>\..\bin\app.exe（再规范化）
//   notepad.exe           → 原样（走 ShellExecute 的 PATH 解析）
//   https://example.com   → 原样
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// KTile.exe 所在目录（不带结尾反斜杠）。取不到时返回空串。
std::wstring executableDirectory();

// 是否是「指向 exe 目录内（或附近）的相对路径」。
//
// 判据刻意窄：必须含目录分隔符、且不是绝对路径、不是 URL。这样
// "notepad.exe"（PATH 里的命令）与 "https://..." 都不会被误当成相对路径。
bool isProgramRelativePath(const std::wstring& path);

// 把相对 exe 目录的路径解析成绝对路径。
//
// 绝对路径、URL、裸命令名一律原样返回（只做环境变量展开与空白裁剪）；
// 相对路径拼到 exe 目录后做 .. / . 规范化。解析结果不要求文件存在 ——
// 启动失败时把绝对路径报出来，比报一句「找不到 .\x.exe」有用得多。
std::wstring resolveProgramRelativePath(const std::wstring& path);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
