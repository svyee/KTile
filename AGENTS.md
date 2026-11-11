# AGENTS.md

本文件面向在本仓库工作的 AI 编码代理（以及人类维护者）。开工前先读一遍；
它的约束优先于“通用最佳实践”。如果本文件与 README 冲突，以本文件的硬约束为准。

## 1. 项目速览

KTile 是一个 Windows 原生磁贴式快速启动器：按热键唤出面板，点击磁贴启动
应用 / 文件 / 文件夹 / 网址。

- 语言与工具链：C++17，Visual Studio 2022 Build Tools（MSVC 14.44），
  直接调用 `cl.exe`（`/std:c++17`）
- 目标平台：Windows 7 SP1 ~ Windows 11（x64）
- 产物：单个 `KTile.exe`，静态链接 `/MT`，约 1.2 MB，无外部运行库
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
- 工具链路径硬编码在 `build.ps1` 顶部（VS2022 BuildTools MSVC 14.44 +
  Windows SDK 10.0.26100.0）。换机器只改那几行。
- `scripts/env-vs2017.ps1` 只用于手工跑 `cl.exe`（如 `tools/` 下的生成器），不参与主构建。
- UI 视觉验证不要依赖屏幕截屏。程序自带调试开关（从仓库根目录运行，
  `data/` 和输出文件都以当前工作目录为基准）：

```powershell
.\build\KTile.exe --debug-shot                 # 渲染到 debug-shot.png 后退出
.\build\KTile.exe --debug-shot=100,100         # 模拟鼠标悬停
.\build\KTile.exe --debug-settings             # 打开设置窗口
.\build\KTile.exe --debug-editor               # 打开第一个磁贴的编辑框
.\build\KTile.exe --debug-add="C:\path\a.lnk"  # 走一次拖入添加流程
.\build\KTile.exe --debug-radial               # 渲染轮盘菜单到 radial-shot.png
.\build\KTile.exe --debug-radial=0,108         # 同上，并高亮正下方扇区（偏移单位 DIP）
```

## 3. 硬约束（违反即视为错误）

### 3.1 只能使用 C++17

编译器是 VS2022 的 MSVC 14.44，但构建固定加 `/std:c++17`，等价于
`_MSVC_LANG = 201703`。**不要顺手改成 c++20**：Win7 底线 + 这份代码的
兼容层（`base/expected.h`、`base/span.h`）都建立在 C++17 上，
换标准等于把两个兼容层和自己的替代物一起推翻。禁止任何 C++20/23 特性：

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
  `winrt::implements` 在当前 SDK + MSVC 14.44 下仍无法正常编译（历史结论，
  换工具链后没有重新验证过，保持手写最省事）。
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
core/           领域层：数据模型、跨度布局、搜索、项目/标签操作、轮盘几何
infrastructure/ 持久化：JSON 解析与写入、便携路径
platform/win32/ 平台层：Win32 / D2D / WIC / 拖放 / 热键 / 图标
ui/             界面层：主窗口、编辑对话框、设置、皮肤、轮盘窗口/编辑器
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
2. **C++20 指定初始化器**：`/std:c++17` 下直接报语法错误。用构造函数。
3. **`winrt::implements`**：无法解析当前 SDK 的 `base.h`，必须手写 COM。
4. **PowerShell 中文乱码**：`build.ps1` 存成无 BOM 后，PS 5.1 会把中文读坏。
5. **`.rc` 中文**：`rc.exe` 编码不可靠，版本资源保持纯 ASCII。
6. **热键手势钩子顺序**：低级键盘钩子是 LIFO，Explorer 重启会插到前面。
   已通过监听 `TaskbarCreated` 重装钩子解决，不要简化掉。
7. **轮盘标签的可用宽度不能用卡片宽度**：花瓣卡片是斜的，能容下的水平文字
   宽度随角度变化（50×48 的卡片在正上方给 48 DIP、斜下方只有 38 DIP）。一律
   走 `core::radialLabelWindowWidth`，绘制层再按它做 `PushAxisAlignedClip`。
   直接按卡片宽度排的后果就是「字跑到按钮外面」（1.2.1 踩过）。
8. **图标路径**：自定义图标 / 内置图标必须存 `data` 相对路径；存绝对路径会
   破坏“拷走即迁移”。唯一的例外是轮盘插件图标：它们在**加载期**从
   `slugin/<id>/` 解析成绝对路径，只存在于内存，从不写进任何 JSON。
9. **测试依赖自动探测**：测试文件 `#include` 的模块名必须能匹配
   `build.ps1` 里的正则，否则依赖源文件不会被编译进去。
10. **调试截图路径**：`--debug-shot` 输出到**当前工作目录**的
    `debug-shot.png`，建议从仓库根目录运行。
11. **`TOOLINFO.cbSize` 必须用 `TTTOOLINFOW_V1_SIZE`（56），不能用
    `sizeof(TOOLINFOW)`（72）**：本程序没有 comctl32 v6 的 manifest，加载的是
    系统自带的 v5.82，它认的还是 IE4 时代那份结构（到 `lpszText` 为止）。
    按 `sizeof` 传 72，`TTM_ADDTOOL` 直接返回 `FALSE` —— 不报错、不写事件日志，
    表现只有「悬停死活不弹」。注意 `TTM_GETTOOLCOUNT` 在 v5 下也不返回真实
    挂载数（恒 0），别拿它当判据；要看效果就 `PrintWindow` 那个
    `tooltips_class32` 窗口截图。
12. **整屏截图在无交互会话里是全黑的**：`BitBlt` 抓屏幕拿不到内容，但
    `PrintWindow` 抓单个窗口正常。UI 验证优先用 `PrintWindow`，不要用桌面截图
    判断「有没有画出来」。下拉列表（`ComboLBox`）不是主窗口的子窗口，主窗口
    `PrintWindow` 抓不到它，得单独抓那个列表窗口再拼回去。
13. **所有者绘制的组合框（`CBS_OWNERDRAWFIXED | CBS_DROPDOWNLIST`）在
    `CB_SETCURSEL(-1)`（无选中项）时仍会收到 `WM_DRAWITEM` 的 `itemID == -1`**：
    所以「字段显示一段和任何条目都无关的汇总文字」是可行的，不用塞一行假条目
    占位。文字与勾选方格都在 `WM_DRAWITEM` 里自己画。
14. **`ComboLBox` 的父窗口不是组合框**：`GetParent(hwndList)` 拿到的是一个内部
    窗口（`GetAncestor(list, GA_ROOT)` 就是它自己），照它去 `CB_SETITEMDATA`
    什么也不会发生 —— 而且不报错，表现是「点了列表不收起、但勾选没变」。要拿
    组合框句柄就 `SetWindowSubclass(hwndList, proc, 1, (DWORD_PTR)combo)`，
    在子类过程里从 `refData` 取。
15. **低级钩子回调会被系统静默跳过**（回调跑在装钩子的线程上，超时即跳过，无
    任何通知）。实测：钩子线程忙等 600ms，期间注入的按下事件 3/3 次都没送到
    回调。后果不是「没反应」，而是**状态机卡在半路**（吞掉了按下却没收到抬起），
    表现为「松开右键后轮盘还挂在屏幕上」。`RadialMouseHook` 为此**把钩子放进
    专用线程**（回调里绝不做耗时活），另外保留三条自愈规则 + `setWheelVisible()`
    回灌。往钩子里加任何可能超时的调用（文件 I/O、进程查询以外的系统调用、
    `SendInput`）之前，先想清楚它会不会把线程占住。
16. **不要在低级钩子回调里调 `SendInput`**：原始输入线程正等着这个回调返回，
    在回调里注入输入有死锁风险。`RadialMouseHook` 用
    `PostThreadMessage(kReinjectMessage)` 回到自己的消息循环里再补发；
    StarPie 的 `ReplayTriggerClick` 也是丢到线程池去做。
17. **调试时只能跑一个 KTile 实例**：两个实例会各自装一个鼠标钩子抢同一批输入
    （事件被两套状态机各处理一遍），还共写同一个日志文件。表现为各种「说不清」
    的假结果——轮盘凭空亮起、日志里出现别的实例写的 `显示 起点 x,y`。`--debug-*`
    探针之前先 `Get-Process KTile | Stop-Process -Force` 并**确认进程数为 1**
    （带 `--debug-settings-keepalive` 开出来的也是一个 KTile 进程，容易漏）。

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
| `src/core/radial_menu.{h,cpp}` | 轮盘动作模型、角度→扇区命中（纯函数，有单测） |
| `src/ui/radial_menu_window.{h,cpp}` | 轮盘花瓣卡片绘制、按角度算的标签窗口与跑马灯、命中测试、出现动画 |
| `src/ui/radial_editor_dialog.{h,cpp}` | 轮盘编辑器（扇区动作 / 进程黑名单） |
| `src/core/radial_plugin.{h,cpp}` | 轮盘插件模型：进程匹配（priority + id 字典序）、插件校验 |
| `src/infrastructure/radial_plugin_store.{h,cpp}` | 扫描 `slugin/<id>/radial.json`，图标相对路径在加载期转绝对 |
| `src/platform/win32/radial_mouse_hook.{h,cpp}` | 鼠标手势状态机（吞键 / 阈值 / 外甩取消） |
| `src/infrastructure/json/radial_store.{h,cpp}` | `radial.json` 读写与 schema 迁移 |
| `src/infrastructure/json/config_store.{h,cpp}` | JSON ↔ 模型、原子读写、损坏备份 |
| `src/ui/launcher_window*.cpp` | 主窗口、命中测试、绘制、交互（体积最大） |
| `src/platform/win32/global_hotkey.{h,cpp}` | 注册热键 + 低级键盘钩子手势 |
| `src/platform/win32/icon_loader.cpp` | Shell / 商店应用图标提取 |
| `src/platform/win32/icon_store.cpp` | 自定义图标导入、缩放、PNG 落盘 |
| `docs/environment-report.md` | 工具链与 Win7 兼容性的实机验证结论 |
| `CHANGELOG.md` | 版本记录与未发布改动 |
