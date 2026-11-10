# 轮盘菜单（Radial Menu）需求与设计文档

> 状态：已实现（v1.2.0）
> 创建日期：2026-11-09
> 最后更新：2026-11-10（按落地情况回填，实现与设计稿的差异集中在「四、4.4」）
> 目标版本：v1.2.0

---

## 一、背景与目标

### 1.1 背景

StarPie 等工具证明了「按住鼠标键拖动 → 弹出扇形菜单 → 松手触发」的轮盘交互在
高频操作场景下效率很高。KTile 作为磁贴快启器，已有完整的 D2D 渲染、全局钩子、
启动分发能力，适合嫁接这个功能。

### 1.2 目标

- 在 KTile 中新增**轮盘菜单**能力：按住指定鼠标键拖动，弹出 4 / 6 / 8 个
  扇区的**花瓣环形菜单**，松开时触发光标所在扇区的动作。
- 支持**按前台应用区分菜单**（Per-App Profile）：不同进程显示不同轮盘。
- 与用户已有的鼠标手势软件（如 MouseInc）**共存**，不抢占对方的触发通道。
- 保持轻量：不引入第三方依赖，体积增量控制在 150KB 以内，单文件 exe。

### 1.3 非目标

- 不做二级子轮盘（StarPie 的级联功能）—— 4~8 个扇区够用，避免复杂度爆炸。
- 不做轨迹手势（画线识别）—— 那是 MouseInc 的领域，不重复造轮子。
- 不做 OCR / 窗口管理等高级动作 —— 动作类型复用 KTile 已有的
  `LaunchItem`（程序 / 文件 / 文件夹 / 网址）+ 自定义快捷键 + 系统内置命令。

---

## 二、与其他鼠标手势软件的冲突分析

### 2.1 冲突根源

MouseInc 等手势软件和本功能都使用 `WH_MOUSE_LL` 低级鼠标钩子，且都以
「按住右键拖动」为触发条件。Windows 低级钩子是链式调用：

- 后安装的钩子排在链首、先执行。
- 钩子回调返回非零值 = 吞掉事件，链上后续钩子和目标窗口都收不到。
- 调用 `CallNextHookEx` = 放行，事件继续传递。

**结论：两个都用右键拖动的工具无法同时正常工作**——谁在链首谁吞事件，
另一个完全收不到右键。这是 Windows 钩子机制的硬限制，无法靠阈值或时序技巧绕开。

### 2.2 共存方案

采用 **方案 A（可配置触发键）+ 方案 B（修饰键门控）组合**：

| 触发模式 | 说明 | 适用场景 |
|---|---|---|
| `Ctrl + 右键拖动` | 按住 Ctrl 再右键拖动才激活轮盘；纯右键拖动放行 | 默认值，与 MouseInc 共存 |
| `Shift + 右键拖动` | 同上，用 Shift | 备选 |
| `Alt + 右键拖动` | 同上，用 Alt | 备选（注意 Alt 在某些软件里有冲突） |
| `右键拖动` | 纯右键拖动激活（吞掉右键） | 不用手势软件的用户 |
| `中键拖动` | 按住鼠标中键拖动激活 | 右键被占用时的首选 |
| `侧键1 / 侧键2 拖动` | 按住鼠标侧键拖动激活 | 有侧键鼠标的用户 |

**默认值为 `Ctrl + 右键拖动`**：保住了右键这个肌肉记忆键，同时通过 Ctrl
修饰键与 MouseInc 的纯右键手势彻底隔离，互不干扰。

### 2.3 判定逻辑

在 `WH_MOUSE_LL` 钩子回调中：

1. 捕获目标鼠标键（右 / 中 / 侧键）的按下事件。
2. 检查修饰键条件（如果配置了 `Ctrl/Shift/Alt + 键`，用 `GetAsyncKeyState`
   确认对应修饰键当前是否按下）。
3. **不满足修饰键条件** → 直接 `CallNextHookEx` 放行，让 MouseInc 等正常工作。
4. **满足修饰键条件** → 吞掉按下事件，进入手势跟踪：
   - 记录起点坐标。
   - 移动距离超过阈值（默认 8px）时，显示轮盘窗口，后续移动更新高亮扇区。
   - 按键抬起时：若已显示轮盘，触发当前高亮扇区动作；若未超阈值，
     重新注入一次原始点击（让原生菜单正常弹出），然后放行。

---

## 三、数据模型

### 3.1 新增结构（`core/radial_menu.h`）

```cpp
namespace ktile::core {

// 轮盘动作类型。不复用 LaunchItem：轮盘需要 Shortcut（注入组合键）这种磁贴
// 没有的类型，硬塞进 LaunchItem 会让两边都长出用不到的字段。执行时按 type
// 分发到 item_launcher / key_injector / system_actions。
enum class RadialActionType : std::uint8_t {
    Application = 0,   // 启动程序
    File        = 1,   // 打开文件
    Folder      = 2,   // 打开文件夹
    Url         = 3,   // 打开网址
    Shortcut    = 4,   // 注入快捷键（修饰键 + 主键）
    Builtin     = 5,   // 内置系统命令（复用 ktile: 前缀）
};

// 单个扇区的动作
struct RadialAction {
    RadialActionType type = RadialActionType::Application;
    std::string name;                          // 显示名
    std::string target;                        // 程序路径 / 文件 / 网址 / 内置命令 id
    std::vector<std::string> arguments;        // 启动参数（仅 Application）
    std::optional<std::string> icon;           // 自定义图标（相对 data 目录）
    std::vector<HotkeyModifier> modifiers;     // 仅 Shortcut 类型有效
    std::string key;                           // 仅 Shortcut 类型有效（如 "C", "F4"）
};

// 一个轮盘方案
struct RadialMenu {
    std::string id;
    std::string name;
    std::uint8_t sectorCount = 8;              // 4 / 6 / 8
    std::vector<RadialAction> actions;         // 固定 sectorCount 个，空槽位留空 action
};

// Per-App 配置：匹配前台进程名 → 使用哪个轮盘
struct RadialProfile {
    std::string processPattern;                // 进程名，如 "chrome.exe"；空串 = 全局默认
    std::string menuId;
};

// 触发方式
enum class RadialTriggerButton : std::uint8_t {
    Right = 0,
    Middle = 1,
    Side1 = 2,
    Side2 = 3,
};

enum class RadialTriggerModifier : std::uint8_t {
    None = 0,    // 纯按键
    Ctrl = 1,
    Shift = 2,
    Alt = 3,
};

struct RadialTriggerConfig {
    bool enabled = true;
    RadialTriggerButton button = RadialTriggerButton::Right;
    RadialTriggerModifier modifier = RadialTriggerModifier::Ctrl;  // 默认 Ctrl+右键
    std::uint16_t activationThresholdPx = 8;   // 拖动超过此距离才呼出轮盘
    std::uint16_t cancelRadiusPx = 160;        // 拖出此半径外松手 = 取消（外甩取消）
};

// 轮盘功能的总配置
struct RadialConfig {
    std::uint32_t schemaVersion = kCurrentSchemaVersion;   // 当前为 2
    RadialTriggerConfig trigger;
    std::vector<RadialMenu> menus;
    std::vector<RadialProfile> profiles;
};

}  // namespace ktile::core
```

### 3.2 持久化

- 独立文件 `data/radial.json`，带 `schemaVersion`（当前为 **2**）。
- 复用 `infrastructure/json` 的 `JsonParser` / `JsonWriter`。
- 损坏时把原文件改名备份为 `.corrupt` 并返回默认值（与 `config.json` 同一套
  容错规则）。
- 原子写：先写 `.tmp` 再替换。
- **没有迁移函数**（有意为之）：字段缺失走默认值、认不出的枚举值回落到安全项、
  越界值在加载时夹取，老文件本来就能读。读成功后统一把 `schemaVersion` 盖成
  当前值——否则下次保存又会把旧值写回去，以后真要做破坏性变更时就没有判断
  依据了。
- 加载时的夹取规则（防止手改的离谱值流进 UI 和窗口）：
  - `activationThresholdPx` < 1 → 1（0 等于「按下即呼出」，会把触发键彻底吃掉）
  - `cancelRadiusPx` 夹到 `[kMinCancelRadiusDip, kMaxCancelRadiusDip]`（142~600 DIP）
  - `sectorCount` 夹到 4~8，`actions` 补齐到 `sectorCount` 个
  - `modifier` 键缺失 → 回落到默认的 `Ctrl`（不是 `None`：`None` 是纯按键拖动，
    会把触发键整个吞掉，手写配置忘了这一项的用户多半不想这样）

**默认数据**：首次运行时写入一个 8 扇区的全局菜单（方案 id `default`，扇区
0 / 2 / 4 / 6 预填复制、粘贴、剪切、撤销，其余留空），外加一条 `processPattern`
为空的全局 profile。空槽位只画一层很淡的填充 + 细轮廓（不画阴影），让环的
骨架看得见但不抢戏。

**单位约定**：`activationThresholdPx` 是**物理像素**（钩子里直接量鼠标位移），
`cancelRadiusPx` 是 **DIP**（花瓣布局本身就是 DIP；按物理像素算的话，150% 缩放
的机器上「取消半径 160」会落进花瓣内部，怎么拖都会取消）。

---

## 四、分层架构与新增模块

遵循 AGENTS.md 的依赖方向规则，只向下依赖。

### 4.1 文件清单（全部已落地）

| 层 | 文件 | 职责 |
|---|---|---|
| core | `radial_menu.h` | 数据模型 + `sectorFromAngle` / `hitTestSector` 声明（纯函数，可单测） |
| core | `radial_menu.cpp` | 角度 → 扇区索引、命中测试、配置校验等纯逻辑 |
| platform/win32 | `foreground_process.h` / `.cpp` | 获取前台窗口的进程名 |
| platform/win32 | `radial_mouse_hook.h` / `.cpp` | `WH_MOUSE_LL` 钩子 + 手势状态机 |
| platform/win32 | `key_injector.h` / `.cpp` | `SendInput` 注入任意修饰键+主键组合 |
| infrastructure/json | `radial_store.h` / `.cpp` | `radial.json` 的读写、越界夹取、损坏备份 |
| ui | `radial_menu_window.h` / `.cpp` | 轮盘窗口（D2D 花瓣绘制 + 命中测试 + 出现动画） |
| ui | `radial_editor_dialog.h` / `.cpp` | 轮盘编辑器（方案管理 / 扇区动作 / 按应用映射） |
| ui | `settings_window.h` / `.cpp` | 设置第 4 页「轮盘」：启用、触发方式、阈值、取消半径 |
| app | `application.h` / `.cpp` | 组装：钩子 → 匹配 profile → 显示窗口 → 分发动作 |
| tests | `test_radial_menu.cpp` | core 层几何与校验单测（18 个用例） |
| tests | `test_persistence.cpp` | 轮盘配置往返、空槽位不错位、越界夹取、版本号盖章 |

### 4.2 各模块设计要点

#### core/radial_menu.cpp
- `std::uint8_t sectorFromAngle(float angleDeg, std::uint8_t sectorCount)`：
  给定角度（0=正上，顺时针）和扇区数，返回扇区索引。**纯函数，必须单测**。
- `std::optional<std::uint8_t> hitTestSector(float dx, float dy, float innerRadius,
  float outerRadius, std::uint8_t sectorCount)`：给定相对中心的位移，
  返回命中的扇区（在内圈或外圈外返回空）。
- `bool validateRadialConfig(const RadialConfig&)`：校验扇区数、动作数量一致等。

#### platform/win32/foreground_process.cpp
```cpp
std::wstring foregroundProcessName();
```
- `GetForegroundWindow` → `GetWindowThreadProcessId` → `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`
  → `QueryFullProcessImageNameW`（Vista+，Win7 可用）→ 取文件名部分。
- 失败返回空串（此时用全局默认菜单）。

#### platform/win32/radial_mouse_hook.cpp
- 仿照 `OutsideClickMonitor` 的钩子安装方式（`WH_MOUSE_LL`，回调在本线程）。
- 状态机：`Idle → Tracking → Shown`，与 `GestureHotkey` 的键盘状态机同构。
- 回调里只做纯内存操作 + `PostMessage` 通知主线程，严禁文件 I/O（钩子超时限制）。
- 修饰键检查用 `GetAsyncKeyState`。
- 外甩取消：拖动距离超过 `cancelRadiusPx` 时进入取消态，松手不触发。

#### platform/win32/key_injector.cpp
- 在现有 `system_actions.cpp` 的 `SendInput` 基础上扩展：
  `void injectShortcut(const std::vector<HotkeyModifier>& modifiers, const std::string& key)`。
- 修饰键映射：`Ctrl → VK_CONTROL`，`Shift → VK_SHIFT`，`Alt → VK_MENU`。
- 主键用 `global_hotkey.cpp` 的 `keyNameToVirtualKey` 转换。

#### ui/radial_menu_window.cpp
- 无边框分层窗口：`WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW`。
- 尺寸约 320×320 DIP，按 DPI 缩放。窗口中心对齐光标位置。
- D2D 绘制：中心核圆 + 4~8 个扇形扇区（`ID2D1PathGeometry` + `AddArc`）+
  图标 + 文字标签。高亮扇区用主题强调色。
- 命中测试用极坐标（角度 + 半径），与 core 层 `hitTestSector` 一致。
- 外观复用 `ThemeColors`，与主窗口视觉统一。
- 点击窗口外自动关闭（可复用 `OutsideClickMonitor` 的思路，但轮盘窗口自己的
  钩子已经在跟踪按键，抬起即关，不需要额外监视器）。

#### app 层组装
- `Application` 持有 `RadialConfig`、`RadialMouseHook`、`RadialMenuWindow`。
- 钩子回调 `PostMessage` 到消息窗口 → `Application::onRadialTrigger(...)`：
  1. 调 `foregroundProcessName()` 取前台进程名。
  2. 在 `RadialConfig::profiles` 中匹配（精确匹配进程名，无匹配用全局默认）。
  3. 找到对应 `RadialMenu`，调用 `RadialMenuWindow::show(menu, cursorPos)`。
- 扇区触发 → `Application::onRadialAction(action)`：按 `RadialActionType` 分发到
  `item_launcher` / `system_actions` / `key_injector`。
- `Application::launchItem` 已有的 `LaunchItem` 执行逻辑可直接复用：把
  `RadialAction` 转成 `LaunchItem` 再调 `launchItem`。

### 4.3 与现有 `OutsideClickMonitor` 的关系

`OutsideClickMonitor` 的 `WH_MOUSE_LL` 钩子是**纯观察**的（不吞事件），用于检测
主窗口外点击。新的 `RadialMouseHook` 是**独立的另一个钩子**，会吞掉满足条件的
鼠标事件。两者互不干扰——观察型钩子总能收到事件（在吞事件型钩子之前或之后，
取决于安装顺序，但观察型不关心顺序，因为它不修改事件流）。

### 4.4 实现与设计稿的偏差（按落地结果回填）

| 项 | 设计稿 | 实际实现 | 原因 |
|---|---|---|---|
| 外观 | 4~8 个扇形（`ID2D1PathGeometry` + `AddArc` 拼圆盘） | **花瓣环**：8 个 50×40 DIP 圆角卡片挂在半径 97 的圆上，中心留空圆 | 扇形切片的文字要跟着旋转才放得下，长中文名会歪；卡片式标签可正向排布，且空槽位的"骨架"更清楚 |
| 扇区数 | 4~8（连续可选） | **4 / 6 / 8** 三档 | 5 / 7 扇区的边界角是无限小数，命中测试的边界判定容易在两个扇区之间抖 |
| 扇区命中 | 极坐标角度 + 半径 | 同（`core::hitTestSector`），但外半径来自 `cancelRadiusPx`（DIP），下限夹到 142 | 滑杆原来不生效；单位必须是 DIP，见 3.2 |
| 设置入口 | 设置页内联「方案列表 + 扇区编辑器 + Per-App」 | 设置页只放触发配置；方案 / 扇区 / 按应用放进**独立对话框**（`radial_editor_dialog`） | 700px 单列设置表单塞不下列表 + 表单 + 映射三重编辑，纵向会溢出 |
| 结果交接 | `SettingsResult` 增加若干轮盘字段 | 交**整份 `RadialConfig`** | 方案 / 扇区 / 规则都是嵌套结构，字段级交接表达不了"删除" |
| 抬起兜底 | 依赖钩子可靠 | 轮盘可见期间挂 **120ms 看门狗定时器**，物理键已松开就强制收起并写日志 | 低级钩子回调会被系统静默跳过，而轮盘窗口 `WS_EX_NOACTIVATE` 没有焦点、收不到 Esc，一旦漏掉抬起就永久挂屏 |
| 侧键区分 | "捕获侧键的按下事件" | 按 `HIWORD(MSLLHOOKSTRUCT.mouseData)` 判定 XBUTTON1 / XBUTTON2，抬起同样校验 | `WM_XBUTTON*` 把两个侧键并成同一消息，只比消息类型会让侧键 1 也触发侧键 2 的轮盘 |
| 矢量图标 | 未提及 | 快捷键（复制 / 粘贴 / 剪切 / 撤销 / 保存）与内置命令走内嵌 Lucide 形状，按**弧长加权质心**居中 | 从 exe 提不到这类动作的图标；按 24×24 外框几何中心摆放时，剪刀、撤销箭头这类不对称图形看起来偏 |
| 缩放 | 未提及 | 整体比初稿小约 10%（窗口半径 158 → 142 DIP） | 142 半径下 8 个花瓣之间留约 10 DIP 缝隙，看出是"一圈"而不是一团 |
| schemaVersion | 初始为 1 | 当前为 **2**，且读成功即盖成当前值，没有迁移函数 | 容错策略（默认值 + 回落 + 夹取）已经能读老文件，再写迁移函数是重复劳动；但版本号必须跟着升，否则以后真做破坏性变更时没有判断依据 |

---

## 五、实现步骤

### 阶段 0：准备（0.5 天）

- [x] `build.ps1`：把将新增的源文件加入 `Get-AppSources`、`Get-CoreSources`
      （core 部分）、`Get-PersistenceSources`（radial_store）。
- [x] `CHANGELOG.md`：在 `[Unreleased]` 下新增「轮盘菜单」条目。

### 阶段 1：core 数据模型 + 单测（1 天）

- [x] 新建 `src/core/radial_menu.h`：定义 `RadialActionType`、`RadialAction`、
      `RadialMenu`、`RadialProfile`、`RadialTriggerConfig`、`RadialConfig`。
- [x] 新建 `src/core/radial_menu.cpp`：实现 `sectorFromAngle`、`hitTestSector`、
      `validateRadialConfig`。
- [x] 新建 `tests/test_radial_menu.cpp`：覆盖角度→扇区映射（4/6/8 扇区）、
      命中测试（内圈/外圈/各扇区边界）、配置校验。
- [x] `build.ps1`：`Get-CoreSources` 加入 `radial_menu.cpp`。
- [x] 运行 `.\build.ps1 -Tests -Test radial`，全部通过。

### 阶段 2：platform 层（1.5 天）

- [x] 新建 `src/platform/win32/foreground_process.{h,cpp}`：`foregroundProcessName()`。
- [x] 新建 `src/platform/win32/key_injector.{h,cpp}`：`injectShortcut`，
      复用 `keyNameToVirtualKey`。
- [x] 新建 `src/platform/win32/radial_mouse_hook.{h,cpp}`：
      - `install(HWND notifyWindow, const RadialTriggerConfig&)`
      - 状态机：Idle / Tracking / Shown
      - 修饰键检查、阈值判定、外甩取消
      - `PostMessage` 通知主线程（自定义消息 `WM_APP + 0x71`）
- [x] `build.ps1`：`Get-AppSources` 加入上述三个 `.cpp`。

### 阶段 3：持久化（0.5 天）

- [x] 新建 `src/infrastructure/json/radial_store.{h,cpp}`：
      `LoadResult<RadialConfig> loadRadial(path)`、`SaveResult saveRadial(path, config)`。
      复用 `JsonParser` / `JsonWriter`，带 `schemaVersion`、损坏备份、原子写。
- [x] `ConfigStore` 或 `Application` 中接入加载 / 保存。
- [x] `build.ps1`：`Get-PersistenceSources` 和 `Get-AppSources` 加入 `radial_store.cpp`。

### 阶段 4：ui 层轮盘窗口（2 天）

- [x] 新建 `src/ui/radial_menu_window.{h,cpp}`：
      - 窗口创建（分层、置顶、不激活）
      - D2D 资源管理（复用 `LauncherWindow` 的设备无关/相关分离模式）
      - 绘制：核圆 + 扇区 + 图标 + 文字 + 高亮
      - 命中测试 + 键盘/鼠标移动更新高亮
      - `show(const RadialMenu&, POINT cursorPos)`、`hide()`、
        `setHighlight(std::optional<uint8_t>)`
- [x] 用 `--debug-shot` 风格的调试开关验证绘制（如 `--debug-radial`）。
- [x] `build.ps1`：`Get-AppSources` 加入 `radial_menu_window.cpp`。

### 阶段 5：app 层组装（1 天）

- [x] `application.h`：新增 `RadialConfig radialConfig_`、
      `RadialMouseHook radialHook_`、`std::unique_ptr<RadialMenuWindow> radialWindow_`。
- [x] `application.cpp`：
      - 启动时加载 `radial.json`。
      - 安装 `RadialMouseHook`。
      - 处理钩子通知消息 → 匹配 profile → 显示轮盘。
      - `onRadialAction` → 分发到 `launchItem` / `injectShortcut` / `runBuiltinAction`。
      - 退出时保存配置。

### 阶段 6：设置界面 + 轮盘编辑器（1.5 天）

设置页（`settings_window` 第 4 页「轮盘」）只放**触发配置**，方案与映射进独立
对话框——700px 单列表单放不下列表 + 表单 + 映射三重编辑（见 4.4）。

- [x] `settings_window` 新增「轮盘」页：启用开关、触发键下拉（右键 / 中键 /
      侧键 1 / 侧键 2）、修饰键下拉（无 / Ctrl / Shift / Alt）、拖动启动阈值滑杆
      （4~40 像素）、外甩取消半径滑杆（150~400 DIP）、底部方案概况摘要、按应用
      的共存说明
- [x] `settings_window.h`：`SettingsResult` 增加 `std::optional<core::RadialConfig>`
      —— 交整份配置而不是零散字段，因为方案 / 扇区 / 规则都是嵌套结构，
      字段级交接表达不了"删除"
- [x] 新建 `radial_editor_dialog`（模态子对话框，双页）：
      - 「扇区动作」页：左侧方案列表（新建 / 删除，方案 id 自动生成）、方案名称、
        扇区数 4 / 6 / 8、扇区列表（扇区 / 类型 / 名称 / 目标）、右侧动作表单
        （6 种类型 + 名称 + 修饰键 + 主键 + 目标 + 选择…按钮）
      - 「按应用」页：规则列表、进程名输入、目标方案下拉、添加 / 更新 / 删除
- [x] 两条不变量在对话框内部强制执行：`actions.size()` 恒等于 `sectorCount`；
      每条 `profile.menuId` 必须指向存在的方案（删方案时连带清掉引用它的规则）
- [x] `application.cpp`：设置返回后重装钩子（触发方式可能变了）、落盘、刷新托盘提示
- [x] `build.ps1`：`Get-AppSources` 加入 `radial_editor_dialog.cpp`

### 阶段 7：验证（1 天）

- [x] `.\build.ps1`：54 个源文件，0 告警，产物 1252 KB
- [x] `.\build.ps1 -Tests`：12 个测试全部通过（`test_radial_menu` 18 个几何/校验
      用例，`test_persistence` 轮盘往返与夹取）
- [x] 绘制验证：`--debug-radial` 导出 PNG + `PrintWindow` 截取，确认花瓣框、图标
      居中、空槽位轮廓、高亮配色
- [x] 设置链路验证：脚本驱动下拉选择 + 点确定，确认 `radial.json` 里触发键
      右键 → 中键、扇区数 8 → 4 均正确落盘
- [ ] 真机鼠标验证（需人工）：默认 Ctrl+右键拖动呼出轮盘、松手触发动作
- [ ] 真机共存验证（需人工）：纯右键拖动不受影响；与 MouseInc 同时运行互不干扰
- [ ] Win7 实机验证：`QueryFullProcessImageNameW`、`SendInput`、分层窗口
      `UpdateLayeredWindow` 路径

### 阶段 8：收尾修复（实现后回归发现）

- [x] 侧键 1 / 侧键 2 无法区分（`WM_XBUTTON*` 合并两个侧键，改按 `mouseData` 高位字判定）
- [x] 抬起事件丢失后轮盘永久挂屏（加 120ms 看门狗定时器，物理键已松开就强制收起）
- [x] 矢量图标偏到左上（D2D 变换矩阵乘法顺序写反，改为"先平移后缩放"）
- [x] 不对称图标与文字错位（改按弧长加权质心居中）
- [x] 高亮时不放大（放大改变了鼠标相对扇区边界的几何关系，扫过边界会抖）
- [x] 屏幕边缘呼出被裁掉一半（窗口夹到显示器工作区，并同步移动光标到实际中心）
- [x] 外甩取消半径滑杆不生效（命中测试用写死的 153 DIP）+ 单位统一为 DIP
- [x] 加载时对阈值 / 取消半径 / 扇区数做越界夹取，`schemaVersion` 读成功即盖章
- [x] 鼠标钩子接入重装链路（TaskbarCreated / 休眠恢复 / 看门狗），与手势钩子共用；
      轮盘显示期间跳过重装，避免截断正在进行的拖动
- [x] 浅色壁纸下空槽位不可见（描边原来是白色，白底等于没画）

---

## 六、Win7 兼容性核查

| API | 最低支持 | 用途 | 结论 |
|---|---|---|---|
| `QueryFullProcessImageNameW` | Vista | 获取前台进程名 | ✅ Win7 可用 |
| `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` | Vista | 打开进程句柄 | ✅ Win7 可用 |
| `SendInput` | XP | 注入快捷键 | ✅ 已在用 |
| `GetAsyncKeyState` | Win95 | 检查修饰键 | ✅ |
| `SetWindowsHookEx(WH_MOUSE_LL)` | Win98 | 鼠标钩子 | ✅ 已在用 |
| `ID2D1DCRenderTarget` + `UpdateLayeredWindow` | D2D 1.0 / Win2000 | 透明分层窗口 | ✅ Win7 可用（这是 Win7 下 D2D 分层窗口拿到干净抗锯齿边缘的标准做法） |
| `ID2D1RenderTarget::DrawRoundedRectangle` | D2D 1.0 (Vista) | 绘制花瓣卡片 | ✅ Win7 自带（最终没用 `PathGeometry::AddArc` 拼扇形） |

所有需要的 API 均在 Win7 SP1 可用范围内，无需引入 Win8+ 接口。

---

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| 低级别钩子被系统静默卸载（超时） | 复用 `GestureHotkey` 的看门狗机制：定期重装 + `TaskbarCreated` 时重装 |
| 修饰键检查时序问题（`GetAsyncKeyState` 与事件队列视图不一致） | 在钩子回调（事件流）里记录修饰键的按下/抬起状态，而不是每次都查物理状态——与 `GestureHotkey` 的 `ctrlPressed_` 同理 |
| 轮盘窗口抢焦点 | `WS_EX_NOACTIVATE` + 显示时不调用 `SetForegroundWindow`，与主窗口一致 |
| 全屏应用中误触 | 新增前台进程黑名单 + 全屏检测（`GetForegroundWindow` + 比较窗口尺寸与屏幕尺寸） |
| 体积超标 | 实测新增 9 个源文件、约 55KB（1197 KB → 1252 KB），远低于 150KB 预算 |

---

## 八、不做的事（明确边界）

1. **不做二级子轮盘**：v1 只做单层 4~8 扇区。
2. **不做轨迹手势识别**：不与 MouseInc 竞争。
3. **不做 OCR / 窗口管理等高级动作**：复用现有动作类型。
4. **不做主题工作室**：轮盘外观跟随主窗口主题，不单独做轮盘主题编辑。
5. **不做手势软件自动检测**：方案 C（自动检测 MouseInc）不实现，靠用户手动选择触发方式。

---

## 九、验收标准

| # | 标准 | 状态 |
|---|---|---|
| 1 | 默认 `Ctrl+右键拖动` 呼出 8 扇区轮盘，松手触发对应动作 | 待人工真机确认 |
| 2 | 纯右键拖动不呼出轮盘，原生右键菜单正常弹出 | 待人工真机确认 |
| 3 | 前台为 `notepad.exe` 时显示记事本专属轮盘，其他程序显示全局轮盘 | ✅ `menuForProcess` 有单测覆盖；真机路径待确认 |
| 4 | 拖出取消半径外松手不触发任何动作 | ✅ 半径已接进命中测试；手感待人工调 |
| 5 | 设置界面可切换触发方式、编辑扇区动作、配置 Per-App 映射 | ✅ 已用脚本驱动点击验证落盘 |
| 6 | `.\build.ps1` 与 `.\build.ps1 -Tests` 全部通过 | ✅ 1252 KB / 0 告警 / 12 个测试全通过 |
| 7 | 与 MouseInc 同时运行时两者功能互不干扰 | 待人工真机确认 |
