# tools — 开发期代码生成

这里的脚本**不参与构建**，只在需要重新生成某些"内嵌"文件时手动跑一次。
产物都已在仓库里，平时开发不需要执行。

| 文件 | 用途 | 产物 |
|---|---|---|
| `gen_lucide_icons.py` | 从 [Lucide](https://lucide.dev) 仓库提取图标形状，生成内嵌 SVG 数据的 C++ 头文件 | `src/core/lucide_icons.h` |
| `gen_app_icon.cpp` | 把应用图标母版转成多尺寸 ico（纯 WIC，无第三方依赖） | `src/app/app.ico` |

## 重新生成应用图标

设计母版是 `assets/icon.png`（正方形、带透明通道）。换图标时只替换母版，
再重新生成 ico —— 窗口、托盘、任务栏、关于对话框、README 全部引用资源
`101`，换 ico 即全局生效。

```powershell
# 在仓库根目录，先注入 VS2017 环境
. .\scripts\env-vs2017.ps1
cl /nologo /EHsc /utf-8 /O2 tools\gen_app_icon.cpp /Fe:build\gen_app_icon.exe `
   /Fo:build\gen_app_icon.obj /link windowscodecs.lib ole32.lib
.\build\gen_app_icon.exe assets\icon.png src\app\app.ico
```

产出 16 / 24 / 32 / 48 / 64 / 128 / 256 七档，每档 PNG 压缩，Win7 起可用。

## 重新生成 Lucide 图标头

```powershell
python tools\gen_lucide_icons.py
```

脚本会把内置系统项用到的 Lucide 图标（含 `transform`、多段 path）转成
`src/core/lucide_icons.h` 里的字符串常量，运行时由
`platform/win32/svg_path.cpp` 解析成 D2D 几何 —— 构建过程不需要 Node 或
任何前端工具链。
