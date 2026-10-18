# 第三方组件声明（Third-Party Notices）

本项目的源代码除特别说明外，均以 [MIT License](LICENSE) 发布。
项目使用了以下第三方资产，在此致谢并附其原始许可：

## Lucide Icons

- 来源：<https://lucide.dev>
- 用途：内置系统项与界面装饰的线框矢量图标（经 `tools/gen_lucide_icons.py`
  转换为 `src/core/lucide_icons.h` 内嵌 SVG 路径数据）
- 许可：ISC License

> ISC License
>
> Copyright (c) for portions of Lucide are held by Brickea, Matt Zuba, and
> individual contributors. Portions Copyright (c) 2020, Lucide Contributors
> Portions Copyright (c) 2010-2020, Cole Bemis, Feather
>
> Permission to use, copy, modify, and/or distribute this software for any
> purpose with or without fee is hereby granted, provided that the above
> copyright notice and this permission notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
> WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
> SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
> WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
> ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
> IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

## 运行环境依赖

以下组件随 Windows 系统提供，不属于本项目分发物，不改变本仓库的许可：

- Microsoft Visual C++ 运行时（静态链接 `/MT`）
- Direct2D / DirectWrite / WIC（系统组件）
