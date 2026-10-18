// KTile - Lucide 图标数据（内嵌 SVG 形状片段）
//
// 来源：Lucide（https://lucide.dev，ISC License），24x24 viewBox、2px 线宽、
// 圆头圆角描边。**本文件由 tools/gen_lucide_icons.py 生成，不要手改**。
//
// 为什么内嵌而不是打包 SVG 文件：Windows 没有系统级 SVG 栅格化器（WIC 不认识
// SVG），而引一个渲染库会让「单 exe 1~2MB」的硬指标失控。Lucide 的形状只是
// 几十个字符的路径数据，编译进 exe 后由 D2D 直接描边 —— 任意 DPI 都清晰，
// 也不需要在 data 目录里放一堆资源文件。
//
// 渲染方式见 platform/win32/svg_path.h：线框图元走描边（圆头圆角），
// fill="currentColor" 的图元走填充。
#pragma once

#include <string>

namespace ktile {
namespace core {

// 取内置项的图标形状片段；id 不认识时返回 nullptr。
inline const wchar_t* lucideBuiltinMarkup(const std::wstring& id) {
    if (id == L"lock") {
        // Lucide: lock
        return L"<rect width=\"18\" height=\"11\" x=\"3\" y=\"11\" rx=\"2\" ry=\"2\" /> <path d=\"M7 11V7a5 5 0 0 1 10 0v4\" />";
    }
    if (id == L"monitor-off") {
        // Lucide: monitor-off
        return L"<path d=\"M12 17v4\" /> <path d=\"M17 17H4a2 2 0 0 1-2-2V5a2 2 0 0 1 1.184-1.826\" /> <path d=\"m2 2 20 20\" /> <path d=\"M8 21h8\" /> <path d=\"M8.656 3H20a2 2 0 0 1 2 2v10a2 2 0 0 1-.293 1.042\" />";
    }
    if (id == L"sleep") {
        // Lucide: moon
        return L"<path d=\"M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401\" />";
    }
    if (id == L"hibernate") {
        // Lucide: moon-star
        return L"<path d=\"M18 5h4\" /> <path d=\"M20 3v4\" /> <path d=\"M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401\" />";
    }
    if (id == L"logoff") {
        // Lucide: log-out
        return L"<path d=\"m16 17 5-5-5-5\" /> <path d=\"M21 12H9\" /> <path d=\"M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4\" />";
    }
    if (id == L"restart") {
        // Lucide: rotate-cw
        return L"<path d=\"M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8\" /> <path d=\"M21 3v5h-5\" />";
    }
    if (id == L"shutdown") {
        // Lucide: power
        return L"<path d=\"M12 2v10\" /> <path d=\"M18.4 6.6a9 9 0 1 1-12.77.04\" />";
    }
    if (id == L"volume-up") {
        // Lucide: volume-2
        return L"<path d=\"M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z\" /> <path d=\"M16 9a5 5 0 0 1 0 6\" /> <path d=\"M19.364 18.364a9 9 0 0 0 0-12.728\" />";
    }
    if (id == L"volume-down") {
        // Lucide: volume-1
        return L"<path d=\"M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z\" /> <path d=\"M16 9a5 5 0 0 1 0 6\" />";
    }
    if (id == L"volume-mute") {
        // Lucide: volume-x
        return L"<path d=\"M11 4.702a.7.7 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.7.7 0 0 0 11 19.298z\" /> <path d=\"m16.5 14.5 5-5\" /> <path d=\"m16.5 9.5 5 5\" />";
    }
    if (id == L"media-prev") {
        // Lucide: skip-back
        return L"<path d=\"M17.971 4.285A2 2 0 0 1 21 6v12a2 2 0 0 1-3.029 1.715l-9.997-5.998a2 2 0 0 1-.003-3.432z\" /> <path d=\"M3 20V4\" />";
    }
    if (id == L"media-next") {
        // Lucide: skip-forward
        return L"<path d=\"M21 4v16\" /> <path d=\"M6.029 4.285A2 2 0 0 0 3 6v12a2 2 0 0 0 3.029 1.715l9.997-5.998a2 2 0 0 0 .003-3.432z\" />";
    }
    if (id == L"this-pc") {
        // Lucide: computer
        return L"<path d=\"M12 18h6\" /> <path d=\"M6 18h.01\" /> <path d=\"M8 6h1\" /> <rect x=\"2\" y=\"14\" width=\"20\" height=\"8\" rx=\"2\" /> <rect x=\"4\" y=\"2\" width=\"16\" height=\"12\" rx=\"2\" />";
    }
    if (id == L"recycle-bin") {
        // Lucide: trash
        return L"<path d=\"M10 11v6\" /> <path d=\"M14 11v6\" /> <path d=\"M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6\" /> <path d=\"M3 6h18\" /> <path d=\"M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2\" />";
    }
    if (id == L"control-panel") {
        // Lucide: settings
        return L"<path d=\"M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915\" /> <circle cx=\"12\" cy=\"12\" r=\"3\" />";
    }
    if (id == L"network") {
        // Lucide: network
        return L"<rect x=\"16\" y=\"16\" width=\"6\" height=\"6\" rx=\"1\" /> <rect x=\"2\" y=\"16\" width=\"6\" height=\"6\" rx=\"1\" /> <rect x=\"9\" y=\"2\" width=\"6\" height=\"6\" rx=\"1\" /> <path d=\"M5 16v-3a1 1 0 0 1 1-1h12a1 1 0 0 1 1 1v3\" /> <path d=\"M12 12V8\" />";
    }
    if (id == L"printers") {
        // Lucide: printer
        return L"<path d=\"M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2\" /> <path d=\"M6 9V3a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v6\" /> <rect x=\"6\" y=\"14\" width=\"12\" height=\"8\" rx=\"1\" />";
    }
    if (id == L"startup-folder") {
        // Lucide: folder-open
        return L"<path d=\"m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2\" />";
    }
    if (id == L"cmd") {
        // Lucide: terminal
        return L"<path d=\"M12 19h8\" /> <path d=\"m4 17 6-6-6-6\" />";
    }
    if (id == L"calc") {
        // Lucide: calculator
        return L"<rect width=\"16\" height=\"20\" x=\"4\" y=\"2\" rx=\"2\" /> <line x1=\"8\" x2=\"16\" y1=\"6\" y2=\"6\" /> <line x1=\"16\" x2=\"16\" y1=\"14\" y2=\"18\" /> <path d=\"M16 10h.01\" /> <path d=\"M12 10h.01\" /> <path d=\"M8 10h.01\" /> <path d=\"M12 14h.01\" /> <path d=\"M8 14h.01\" /> <path d=\"M12 18h.01\" /> <path d=\"M8 18h.01\" />";
    }
    if (id == L"notepad") {
        // Lucide: notepad-text
        return L"<path d=\"M8 2v4\" /> <path d=\"M12 2v4\" /> <path d=\"M16 2v4\" /> <rect width=\"16\" height=\"18\" x=\"4\" y=\"4\" rx=\"2\" /> <path d=\"M8 10h6\" /> <path d=\"M8 14h8\" /> <path d=\"M8 18h5\" />";
    }
    if (id == L"mspaint") {
        // Lucide: palette
        return L"<path d=\"M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8z\" /> <circle cx=\"13.5\" cy=\"6.5\" r=\".5\" fill=\"currentColor\" /> <circle cx=\"17.5\" cy=\"10.5\" r=\".5\" fill=\"currentColor\" /> <circle cx=\"6.5\" cy=\"12.5\" r=\".5\" fill=\"currentColor\" /> <circle cx=\"8.5\" cy=\"7.5\" r=\".5\" fill=\"currentColor\" />";
    }
    if (id == L"hosts") {
        // Lucide: file-text
        return L"<path d=\"M6 22a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h8a2.4 2.4 0 0 1 1.704.706l3.588 3.588A2.4 2.4 0 0 1 20 8v12a2 2 0 0 1-2 2z\" /> <path d=\"M14 2v5a1 1 0 0 0 1 1h5\" /> <path d=\"M10 9H8\" /> <path d=\"M16 13H8\" /> <path d=\"M16 17H8\" />";
    }
    if (id == L"media-playpause") {
        // Lucide: play（缩小） + pause 的两根竖线
        return L"<path transform=\"translate(0.1 4.1) scale(0.66)\" d=\"M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z\" /><line x1=\"16.5\" x2=\"16.5\" y1=\"5\" y2=\"19\" /><line x1=\"20.5\" x2=\"20.5\" y1=\"5\" y2=\"19\" />";
    }
    return nullptr;
}

// 主窗口右上角的「钉住」按钮（不在内置项表里，单独一份）
inline const wchar_t* lucidePinMarkup() {
    // Lucide: pin
    return L"<path d=\"M12 17v5\" /> <path d=\"M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z\" />";
}

}  // namespace core
}  // namespace ktile
