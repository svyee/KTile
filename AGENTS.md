# AGENTS.md

本文件面向在本仓库工作的 AI 编码代理（以及人类维护者）。开工前先读一遍；
它的约束优先于“通用最佳实践”。如果本文件与 README 冲突，以本文件的硬约束为准。

## 1. 项目速览

KTile 是一个 Windows 原生磁贴式快速启动器：按热键唤出面板，点击磁贴启动
应用 / 文件 / 文件夹 / 网址。

- 语言与工具链：C++17，Visual Studio 2017（MSVC 14.16），直接调用 `cl.exe`
- 目标平台：Windows 7 SP1 ~ Windows 11（x64）
- 产物：单个 `KTile.exe`，静态链接 `/MT`，约 800 KB，无外部运行库
- 运行时数据：exe 同级的 `data/`（`items.json`、`config.json`、`themes/`、`res/`）
- 第三方依赖：**没有**。JSON 自研，UI 只用系统自带的 Direct2D / DirectWrite / WIC

## 2. 常用命令

在仓库根目录执行（PowerShell）：

```powershell
.\build.ps1                  # Release 构建，产物 build/KTile.exe
.\build.ps1 -Debug           # Debug 构建
.\build.ps1 -Tests           # 编译并运行全部单元测试
.\build.ps1 -Tests -Test tile  # 只跑文件名含 "tile" 的测试
.\build.ps1 -Clean           # 清空 build/
```

- 不需要先跑 `vcvars64.bat`，`build.ps1` 自己注入 `PATH` / `INCLUDE` / `LIB`。
- 工具链路径硬编码在 `build.ps1` 顶部（VS2017 Community + SDK 10.0.19041.0）。
- `scripts/env-vs2017.ps1` 只用于手工跑 `cl.exe`（如 `tools/` 下的生成器），不参与主构建。
- UI 视觉验证不要依赖屏幕截屏。程序自带调试开关（从仓库根目录运行，
  `data/` 和输出文件都以当前工作目录为基准）：

```powershell
.\build\KTile.exe --debug-shot                 # 渲染到 debug-shot.png 后退出
.\build\KTile.exe --debug-shot=100,100         # 模拟鼠标悬停
.\build\KTile.exe --debug-settings             # 打开设置窗口
.\build\KTile.exe --debug-editor               # 打开第一个磁贴的编辑框
.\build\KTile.exe --debug-add="C:\path\a.lnk"  # 走一次拖入添加流程
```

## 3. 硬约束（违反即视为错误）

### 3.1 只能使用 C++17

VS2017 的 MSVC 19.16 是上限，`_MSVC_LANG = 201703`。禁止任何 C++20/23 特性：

| 禁止 | 替代方案 |
|---|---|
| 指定初始化器 `.x = 1` | 显式构造函数，或逐字段赋值 |
| `std::expected` | `src/base/expected.h` 的 `Expected<T, E>` |
| `std::span` | `src/base/span.h`，或 `const std::vector<T>&` + 下标 |
| `std::ranges` / `views` | 传统 `<algorithm>` + 迭代器 |
| `concepts` / `requires` | `if constexpr` + SFINAE |
| `std::format` / `starts_with` / `contains` | 现有字符串工具或手写比较 |
| modules / coroutines | 不使用 |

可用：结构化绑定、`if constexpr`、`inline` 变量、`std::optional`、`std::string_view`、
`std::filesystem`（已实测可用）。

### 3.2 Win7 SP1 是底线

- 编译宏固定为 `_WIN32_WINNT=0x0601` / `WINVER=0x0601`。
- 不要使用 Windows 8+ 才有的 API。典型雷区：`SetProcessDpiAwareness` /
  `SetProcessDpiAwarenessContext`、`RoInitialize`、`RoGetActivationFactory`、
  `GetDpiForWindow`、`D2D1CreateDevice` 等。不确定时查 MSDN 的
  “Minimum supported client”，或用 `dumpbin /imports build\KTile.exe` 验证导入表。
- DPI 感知使用 `SetProcessDPIAware()`（Vista+），不要替换成 Per-Monitor 系列。
- COM 类**手写**（参考 `src/platform/win32/drop_target.cpp`）。
  `winrt::implements` 在当前 SDK + VS2017 下无法编译。
- `winrt::com_ptr` 可以继续用（实测不引入 Win8+ 导入）。

### 3.3 单文件、零依赖

- 保持 `/MT` 静态链接；不要改成 `/MD`。
- 不引入 vcpkg / NuGet / 第三方源码库。`third_party/` 为空是刻意的。
- 新增系统库前先确认 Win7 自带，并同步 `build.ps1` 的 `$SystemLibs`。
- 不迁移到 CMake / Meson；`build.ps1` 是唯一构建入口。
- 体积是产品特性。不要为了“代码优雅”引入大型头文件或模板库。

### 3.4 新增源文件必须同步构建脚本

`build.ps1` 使用显式源文件列表，不扫描目录：

- 新增 `src/**/*.cpp` → 加入 `Get-AppSources`，否则 exe 根本不会链接它。
- 新增被测试使用的 core 模块 → 加入 `Get-CoreSources`。
- 新增被测试使用的 infrastructure 模块 → 加入 `Get-PersistenceSources`。
- 新增测试依赖的模块若不在现有自动探测规则里 → 在 `Tests` 分支补一条
  `if ($content -match "...")` 规则。
- 删除 / 重命名源文件时同步清理这些列表。

### 3.5 文件编码

- `build.ps1` 必须保存为 **UTF-8 with BOM**（PowerShell 5.1 否则误读中文）。
- `.cpp` / `.h` 使用 UTF-8（当前无 BOM），编译参数已带 `/utf-8`。
- `src/app/app.rc` 必须保持**纯 ASCII**（`rc.exe` 编码坑），中文文案只放对话框。
- 不修改 `.gitattributes` 的文本自动换行策略。

### 3.6 仓库卫生

- 不提交 `build/`、`data/`（`data/themes/*.json` 例外）、`*.exe`、`*.obj`。
- 不提交真实用户数据、绝对路径配置、日志和截图产物。

## 4. 分层架构与依赖方向

```
base/           C++17 兼容层 + 编码转换（expected / span / algo / text_convert）
core/           领域层：数据模型、跨度布局、搜索、项目/标签操作
infrastructure/ 持久化：JSON 解析与写入、便携路径
platform/win32/ 平台层：Win32 / D2D / WIC / 拖放 / 热键 / 图标
ui/             界面层：主窗口、编辑对话框、设置、皮肤
app/            组装与生命周期
```

依赖规则（只能向下，不能反向）：

- `base`：头文件只依赖标准库；`text_convert.cpp` 可使用 Win32 编码 API。
- `core`：**只依赖标准库**。禁止 `#include <Windows.h>`、禁止 JSON / UI / Win32。
  这是可单测的前提。纯逻辑优先放这里。
- `infrastructure`：可依赖 `core` + 标准库；文件系统细节可在 `.cpp` 用 Win32。
- `platform/win32`：可依赖 `core` / `base` / Win32；禁止依赖 `ui` / `app`。
- `ui`：可依赖 `base` / `core` / `platform` / `infrastructure`；禁止依赖 `app`。
- `app`：唯一可以组装所有层的层。

判断标准：新增逻辑若不需要窗口、不需要 Win32 句柄，就应放 `core` 并补单元测试。

## 5. 代码约定

- 命名空间：`ktile::base`、`ktile::core`、`ktile::infrastructure`、
  `ktile::platform::win32`、`ktile::ui`、`ktile::app`。
- 命名：类型 / 枚举值 `PascalCase`；函数 `camelCase`；成员变量尾随下划线
  （`columns_`）；常量 `kPascalCase`（`kDefaultGridColumns`）。
- 头文件用 `#pragma once`；include 顺序：本项目头（`"core/xxx.h"`，相对 `src/`）
  → Windows / 系统头 → 标准库。
- 头文件禁止 `using namespace`；`.cpp` 中慎用。
- 注释使用中文，重点写“为什么这样设计 / 为什么不能那样”，而不是复述代码。
  文件头通常包含职责、设计取舍和被否决的方案。
- 结构体用默认成员初始化器 + 显式构造函数，不要依赖聚合初始化。
- 简短的 getter / 明确无异常的函数标 `noexcept`。
- 错误处理：平台层返回 `Win32Result<T>` / `Win32Status`（见
  `src/platform/win32/win32_error.h`）；不要跨层抛异常。
- 资源管理：句柄用 `src/platform/win32/unique_handles.h` 的 RAII 封装，
  不写裸 `CloseHandle` 分支。
- 字符串：JSON / 领域模型内部用 UTF-8 `std::string`；Win32 / DirectWrite 用
  UTF-16 `std::wstring`；转换统一走 `base/text_convert.h`。
- 颜色统一用 ARGB `std::uint32_t`。
- 路径存储：图标 / 背景图等存在 `data` 下的资源一律存**相对 data 目录**的路径，
  保证整个目录可搬移。

## 6. 测试约定

- 测试文件：`tests/test_<模块>.cpp`，一个模块一个文件。
- 不使用 gtest / Catch；每个文件自带 `CHECK` 宏、`wmain()`，失败计数作为退出码。
- 新增 / 修改 `core` 纯逻辑必须补测试；修 bug 先写能复现的用例。
- `build.ps1 -Tests` 会根据测试文件的 `#include` 自动附带依赖源文件，
  新模块若不被识别，会出现链接错误——此时补 3.4 中的探测规则。
- UI 与 Win32 交互不做单元测试；用第 2 节的 `--debug-*` 开关做视觉 / 流程验证。

## 7. 数据模型与兼容性

- 数据文件：`data/items.json`、`data/config.json`，均带 `schemaVersion`
  （当前 `kCurrentSchemaVersion = 1`，见 `src/core/data_model.h`）。
- 修改字段或语义时：递增 `schemaVersion`，并在 `ConfigStore` 的反序列化路径
  提供迁移 / 默认值，保证旧文件仍能加载。
- 枚举写入 JSON 时使用稳定字符串键（如 `"hover"` / `"doubleCtrl"`），
  不要写整数——增删枚举值时不会串位。
- 读取容错：文件不存在 = 首次运行，返回默认值；文件损坏 = 备份为 `.bak`
  后返回默认值。不要改成静默覆盖。
- 保存必须是原子写（先写 `.tmp` 再替换），不要直接截断原文件。
- 不要删除或改名 `config.json` / `items.json` 的既有字段；新增字段必须带默认值。

## 8. UI / 渲染注意事项

- 只使用 Direct2D 1.0 + DirectWrite + WIC（Win7 自带范围），不要引入 D2D 效果
  或需要 Platform Update 的接口。
- 绘制每帧先 Clear 到不透明底色，再做半透明合成，避免残影。
- 窗口与磁贴不透明度由分层窗口 / 逐项 alpha 实现，不要用 GDI 做透明。
- 所有坐标按 `dpiScale()`（`LOGPIXELSX / 96`）换算；不要写死像素。
- 磁贴布局必须保持确定性：相同输入产生相同布局，避免界面抖动。
- 拖拽采用“插入式重排 / 最小扰动避让”，相关规则集中在
  `src/core/tile_layout.{h,cpp}`，不要在 UI 层重复实现一套。

## 9. 文档与发布

- 面向用户的改动必须在 `CHANGELOG.md` 的 `[Unreleased]` 下补充一条，
  分类为 `新增` / `修复` / `变更`。
- 发版时同步三处版本号：
  - `src/app/application.h` 的 `kApplicationVersion*`
  - `src/app/app.rc` 的 `FILEVERSION` / `PRODUCTVERSION`
  - `src/app/app.rc` 的 `"FileVersion"` / `"ProductVersion"` 字符串
- 新增顶层目录或模块时更新 `README.md` 的目录树。
- 文档放 `docs/`，设计母版放 `assets/`，开发期生成器放 `tools/`（不参与构建）。
- 不要手动编辑 `src/core/lucide_icons.h` / `src/app/app.ico`；需要时跑
  `tools/` 下的生成器。

## 10. 提交前检查清单

- [ ] `.\build.ps1` 通过，无新增编译警告（项目使用 `/W4 /permissive-`）
- [ ] `.\build.ps1 -Tests` 全部通过
- [ ] 新增 `.cpp` 已加入 `build.ps1` 对应列表
- [ ] 未使用 C++20 / Win8+ API / 新增第三方依赖
- [ ] 用户可见改动已写入 `CHANGELOG.md`
- [ ] 数据模型变更已递增 `schemaVersion` 并提供迁移
- [ ] 新资源路径使用 data 目录相对路径
- [ ] 未混入 `build/`、`data/`、日志或截图产物

## 11. 已知陷阱（都是实测踩过的）

1. **忘加 `build.ps1` 源文件列表**：编译“成功”但 exe 里没有新代码，或链接报
   `unresolved external symbol`。新增 `.cpp` 后第一件事就是更新列表。
2. **C++20 指定初始化器**：VS2017 直接报语法错误。用构造函数。
3. **`winrt::implements`**：VS2017 无法解析当前 SDK 的 `base.h`，必须手写 COM。
4. **PowerShell 中文乱码**：`build.ps1` 存成无 BOM 后，PS 5.1 会把中文读坏。
5. **`.rc` 中文**：`rc.exe` 编码不可靠，版本资源保持纯 ASCII。
6. **热键手势钩子顺序**：低级键盘钩子是 LIFO，Explorer 重启会插到前面。
   已通过监听 `TaskbarCreated` 重装钩子解决，不要简化掉。
7. **图标路径**：自定义图标 / 内置图标必须存 `data` 相对路径；存绝对路径会
   破坏“拷走即迁移”。
8. **测试依赖自动探测**：测试文件 `#include` 的模块名必须能匹配
   `build.ps1` 里的正则，否则依赖源文件不会被编译进去。
9. **调试截图路径**：`--debug-shot` 输出到**当前工作目录**的
   `debug-shot.png`，建议从仓库根目录运行。

## 12. 关键文件导航

| 文件 | 作用 |
|---|---|
| `build.ps1` | 唯一构建 / 测试入口，含源文件列表与工具链路径 |
| `src/app/application.{h,cpp}` | 应用组装、生命周期、托盘 / 热键 / 持久化协调 |
| `src/core/data_model.h` | 领域模型、枚举、配置结构、schema 版本 |
| `src/core/tile_layout.{h,cpp}` | 跨度布局、拖拽重排、避让算法（纯函数） |
| `src/core/item_operations.{h,cpp}` | 磁贴增删改、尺寸 / 坐标一致性 |
| `src/core/tab_operations.{h,cpp}` | 标签页增删改与排序 |
| `src/core/search.{h,cpp}` | 子串搜索与高亮区间 |
| `src/infrastructure/json/config_store.{h,cpp}` | JSON ↔ 模型、原子读写、损坏备份 |
| `src/ui/launcher_window*.cpp` | 主窗口、命中测试、绘制、交互（体积最大） |
| `src/platform/win32/global_hotkey.{h,cpp}` | 注册热键 + 低级键盘钩子手势 |
| `src/platform/win32/icon_loader.cpp` | Shell / 商店应用图标提取 |
| `src/platform/win32/icon_store.cpp` | 自定义图标导入、缩放、PNG 落盘 |
| `docs/environment-report.md` | 工具链与 Win7 兼容性的实机验证结论 |
| `CHANGELOG.md` | 版本记录与未发布改动 |
