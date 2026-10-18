#!/usr/bin/env python3
"""从 Lucide 图标库生成 src/core/lucide_icons.h（内嵌 SVG 形状片段）。

用法：
    python tools/gen_lucide_icons.py E:\\Downloads\\lucide-main

什么时候需要跑：想换某个图标、或整体升级 Lucide 版本时。产物
（src/core/lucide_icons.h）已提交到仓库，**构建过程不依赖本脚本**，
也不依赖 node / npm —— 这是刻意的：KTile 的构建环境只有 VS2017。

生成的头文件只保留每个图标的形状元素（path/rect/circle/line），
24x24 viewBox、2px 线宽、圆头圆角这些风格属性由渲染方统一施加
（见 platform/win32/svg_path.h）。
"""

import re
import sys
from pathlib import Path

# 内置项 id -> Lucide 图标名（顺序与 core/builtin_items.h 的分组一致）
ICON_MAP = [
    ("lock", "lock"),
    ("monitor-off", "monitor-off"),
    ("sleep", "moon"),
    ("hibernate", "moon-star"),
    ("logoff", "log-out"),
    ("restart", "rotate-cw"),
    ("shutdown", "power"),
    ("volume-up", "volume-2"),
    ("volume-down", "volume-1"),
    ("volume-mute", "volume-x"),
    ("media-prev", "skip-back"),
    ("media-next", "skip-forward"),
    ("this-pc", "computer"),
    # 新版 Lucide 把 trash-2 并回了 trash（老版本的 trash-2 就是现在这个形状）
    ("recycle-bin", "trash"),
    ("control-panel", "settings"),
    ("network", "network"),
    ("printers", "printer"),
    ("startup-folder", "folder-open"),
    ("cmd", "terminal"),
    ("calc", "calculator"),
    ("notepad", "notepad-text"),
    ("mspaint", "palette"),
    ("hosts", "file-text"),
]

# Lucide 没有「播放/暂停」合并图标，用 play 缩小后与 pause 的两根竖线组合。
# 缩放在渲染方支持（transform="translate(...) scale(...)"）。
PLAY_PAUSE = (
    '<path transform="translate(0.1 4.1) scale(0.66)" '
    'd="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458'
    'l-12 7A2 2 0 0 1 5 19z" />'
    '<line x1="16.5" x2="16.5" y1="5" y2="19" />'
    '<line x1="20.5" x2="20.5" y1="5" y2="19" />'
)

HEADER = """// KTile - Lucide 图标数据（内嵌 SVG 形状片段）
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
"""

FOOTER = """    return nullptr;
}

// 主窗口右上角的「钉住」按钮（不在内置项表里，单独一份）
inline const wchar_t* lucidePinMarkup() {
    // Lucide: pin
    return L"%(pin)s";
}

}  // namespace core
}  // namespace ktile
"""


def read_shapes(svg_path: Path) -> str:
    text = svg_path.read_text(encoding="utf-8")
    # 去掉 XML 声明与最外层 <svg>...</svg>
    text = re.sub(r"<\?xml.*?\?>", "", text, flags=re.S)
    text = re.sub(r"<svg\b.*?>", "", text, count=1, flags=re.S)
    text = re.sub(r"</svg\s*>", "", text)
    # 压成一行：去掉换行与行首缩进，保留属性之间的单个空格
    text = re.sub(r"\s*\n\s*", " ", text)
    text = re.sub(r"\s{2,}", " ", text).strip()
    return text


def cpp_literal(shapes: str) -> str:
    return shapes.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: gen_lucide_icons.py <lucide-repo-root>", file=sys.stderr)
        return 2
    icons_dir = Path(sys.argv[1]) / "icons"
    if not icons_dir.is_dir():
        print(f"icons dir not found: {icons_dir}", file=sys.stderr)
        return 2

    repo_root = Path(__file__).resolve().parent.parent
    out_path = repo_root / "src" / "core" / "lucide_icons.h"

    body = []
    for item_id, lucide_name in ICON_MAP:
        svg_path = icons_dir / f"{lucide_name}.svg"
        if not svg_path.is_file():
            print(f"missing icon: {lucide_name}", file=sys.stderr)
            return 1
        shapes = cpp_literal(read_shapes(svg_path))
        body.append(
            f'    if (id == L"{item_id}") {{\n'
            f"        // Lucide: {lucide_name}\n"
            f'        return L"{shapes}";\n'
            f"    }}"
        )

    # 组合图标（没有对应的 Lucide 文件，单列在表尾）
    body.append(
        "    if (id == L\"media-playpause\") {\n"
        "        // Lucide: play（缩小） + pause 的两根竖线\n"
        f'        return L"{cpp_literal(PLAY_PAUSE)}";\n'
        "    }"
    )

    pin_shapes = cpp_literal(read_shapes(icons_dir / "pin.svg"))

    text = HEADER + "\n".join(body) + "\n" + FOOTER % {"pin": pin_shapes}
    out_path.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out_path} ({len(text)} bytes, {len(ICON_MAP) + 1} icons)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
