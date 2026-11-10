# 轮盘菜单（Radial Menu）需求与设计文档

> 状态：设计稿
> 创建日期：2026-11-09
> 目标版本：v1.2.0

---

## 一、背景与目标

### 1.1 背景

StarPie 等工具证明了「按住鼠标键拖动 → 弹出扇形菜单 → 松手触发」的轮盘交互在
高频操作场景下效率很高。KTile 作为磁贴快启器，已有完整的 D2D 渲染、全局钩子、
启动分发能力，适合嫁接这个功能。

### 1.2 目标

- 在 KTile 中新增**轮盘菜单**能力：按住指定鼠标键拖动，弹出 4~8 个扇区的
  圆形菜单，松开时触发光标所在扇区的动作。
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

// 轮盘动作类型。复用 LaunchItem 的已有类型，新增 Shortcut（自定义组合键）。
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
    RadialTriggerConfig trigger;
    std::vector<RadialMenu> menus;
    std::vector<RadialProfile> profiles;
};

}  // namespace ktile::core
```

### 3.2 持久化

- 独立文件 `data/radial.json`，带 `schemaVersion`（初始为 1）。
- 复用 `infrastructure/json` 的 `JsonParser` / `JsonWriter`。
- 损坏时备份为 `.bak` 并返回默认值（与 `config.json` 同一套容错规则）。
- 原子写：先写 `.tmp` 再替换。

**默认数据**：首次运行时写入一个 8 扇区的全局菜单，扇区动作可留空（显示「+」
占位，点击后由设置界面编辑）。

---

## 四、分层架构与新增模块

遵循 AGENTS.md 的依赖方向规则，只向下依赖。

### 4.1 新增文件清单

| 层 | 文件 | 职责 |
|---|---|---|
| core | `radial_menu.h` | 数据模型 + 扇区命中计算（纯函数，可单测） |
| core | `radial_menu.cpp` | 角度 → 扇区索引、配置校验等纯逻辑 |
| platform/win32 | `foreground_process.h` / `.cpp` | 获取前台窗口的进程名 |
| platform/win32 | `radial_mouse_hook.h` / `.cpp` | `WH_MOUSE_LL` 钩子 + 手势状态机 |
| platform/win32 | `key_injector.h` / `.cpp` | `SendInput` 注入任意修饰键+主键组合 |
| infrastructure/json | `radial_store.h` / `.cpp` | `radial.json` 的读写 + 迁移 |
| ui | `radial_menu_window.h` / `.cpp` | 轮盘窗口（D2D 绘制扇区 + 命中测试） |
| app | （修改 `application.{h,cpp}`） | 组装：钩子 → 匹配 profile → 显示窗口 → 分发动作 |
| ui | （修改 `settings_window.{h,cpp}`） | 设置页：触发方式 + 轮盘编辑 |
| tests | `test_radial_menu.cpp` | core 层纯逻辑单测 |

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

---

## 五、实现步骤

### 阶段 0：准备（0.5 天）

- [ ] `build.ps1`：把将新增的源文件加入 `Get-AppSources`、`Get-CoreSources`
      （core 部分）、`Get-PersistenceSources`（radial_store）。
- [ ] `CHANGELOG.md`：在 `[Unreleased]` 下新增「轮盘菜单」条目。

### 阶段 1：core 数据模型 + 单测（1 天）

- [ ] 新建 `src/core/radial_menu.h`：定义 `RadialActionType`、`RadialAction`、
      `RadialMenu`、`RadialProfile`、`RadialTriggerConfig`、`RadialConfig`。
- [ ] 新建 `src/core/radial_menu.cpp`：实现 `sectorFromAngle`、`hitTestSector`、
      `validateRadialConfig`。
- [ ] 新建 `tests/test_radial_menu.cpp`：覆盖角度→扇区映射（4/6/8 扇区）、
      命中测试（内圈/外圈/各扇区边界）、配置校验。
- [ ] `build.ps1`：`Get-CoreSources` 加入 `radial_menu.cpp`。
- [ ] 运行 `.\build.ps1 -Tests -Test radial`，全部通过。

### 阶段 2：platform 层（1.5 天）

- [ ] 新建 `src/platform/win32/foreground_process.{h,cpp}`：`foregroundProcessName()`。
- [ ] 新建 `src/platform/win32/key_injector.{h,cpp}`：`injectShortcut`，
      复用 `keyNameToVirtualKey`。
- [ ] 新建 `src/platform/win32/radial_mouse_hook.{h,cpp}`：
      - `install(HWND notifyWindow, const RadialTriggerConfig&)`
      - 状态机：Idle / Tracking / Shown
      - 修饰键检查、阈值判定、外甩取消
      - `PostMessage` 通知主线程（自定义消息 `WM_APP + 0x71`）
- [ ] `build.ps1`：`Get-AppSources` 加入上述三个 `.cpp`。

### 阶段 3：持久化（0.5 天）

- [ ] 新建 `src/infrastructure/json/radial_store.{h,cpp}`：
      `LoadResult<RadialConfig> loadRadial(path)`、`SaveResult saveRadial(path, config)`。
      复用 `JsonParser` / `JsonWriter`，带 `schemaVersion`、损坏备份、原子写。
- [ ] `ConfigStore` 或 `Application` 中接入加载 / 保存。
- [ ] `build.ps1`：`Get-PersistenceSources` 和 `Get-AppSources` 加入 `radial_store.cpp`。

### 阶段 4：ui 层轮盘窗口（2 天）

- [ ] 新建 `src/ui/radial_menu_window.{h,cpp}`：
      - 窗口创建（分层、置顶、不激活）
      - D2D 资源管理（复用 `LauncherWindow` 的设备无关/相关分离模式）
      - 绘制：核圆 + 扇区 + 图标 + 文字 + 高亮
      - 命中测试 + 键盘/鼠标移动更新高亮
      - `show(const RadialMenu&, POINT cursorPos)`、`hide()`、
        `setHighlight(std::optional<uint8_t>)`
- [ ] 用 `--debug-shot` 风格的调试开关验证绘制（如 `--debug-radial`）。
- [ ] `build.ps1`：`Get-AppSources` 加入 `radial_menu_window.cpp`。

### 阶段 5：app 层组装（1 天）

- [ ] `application.h`：新增 `RadialConfig radialConfig_`、
      `RadialMouseHook radialHook_`、`std::unique_ptr<RadialMenuWindow> radialWindow_`。
- [ ] `application.cpp`：
      - 启动时加载 `radial.json`。
      - 安装 `RadialMouseHook`。
      - 处理钩子通知消息 → 匹配 profile → 显示轮盘。
      - `onRadialAction` → 分发到 `launchItem` / `injectShortcut` / `runBuiltinAction`。
      - 退出时保存配置。

### 阶段 6：设置界面（1.5 天）

- [ ] `settings_window` 新增「轮盘菜单」页：
      - 启用开关
      - 触发方式下拉（右键 / 中键 / 侧键1 / 侧键2）+ 修饰键下拉（无 / Ctrl / Shift / Alt）
      - 阈值滑块
      - 轮盘方案列表（新建 / 重命名 / 删除）
      - 扇区编辑器（4/6/8 切换 + 每个扇区的动作编辑，复用 `item_editor_dialog` 的
        目标选择逻辑）
      - Per-App profile 映射（进程名 → 轮盘方案）
- [ ] `SettingsResult` 新增轮盘相关字段。

### 阶段 7：验证（1 天）

- [ ] `.\build.ps1` 编译通过，无新增警告。
- [ ] `.\build.ps1 -Tests` 全部通过。
- [ ] 真机验证：
      - 默认 Ctrl+右键拖动呼出轮盘，松手触发动作。
      - 纯右键拖动不受影响（原生右键菜单正常）。
      - 不同前台应用显示不同轮盘。
      - 与 MouseInc 同时运行：MouseInc 纯右键手势正常，KTile Ctrl+右键轮盘正常。
      - 外甩取消生效。
      - Win7 兼容：`QueryFullProcessImageNameW`、`SendInput`、D2D 扇形绘制均正常。

---

## 六、Win7 兼容性核查

| API | 最低支持 | 用途 | 结论 |
|---|---|---|---|
| `QueryFullProcessImageNameW` | Vista | 获取前台进程名 | ✅ Win7 可用 |
| `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` | Vista | 打开进程句柄 | ✅ Win7 可用 |
| `SendInput` | XP | 注入快捷键 | ✅ 已在用 |
| `GetAsyncKeyState` | Win95 | 检查修饰键 | ✅ |
| `SetWindowsHookEx(WH_MOUSE_LL)` | Win98 | 鼠标钩子 | ✅ 已在用 |
| `ID2D1PathGeometry::AddArc` | D2D 1.0 (Vista) | 绘制扇形 | ✅ Win7 自带 |

所有需要的 API 均在 Win7 SP1 可用范围内，无需引入 Win8+ 接口。

---

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| 低级别钩子被系统静默卸载（超时） | 复用 `GestureHotkey` 的看门狗机制：定期重装 + `TaskbarCreated` 时重装 |
| 修饰键检查时序问题（`GetAsyncKeyState` 与事件队列视图不一致） | 在钩子回调（事件流）里记录修饰键的按下/抬起状态，而不是每次都查物理状态——与 `GestureHotkey` 的 `ctrlPressed_` 同理 |
| 轮盘窗口抢焦点 | `WS_EX_NOACTIVATE` + 显示时不调用 `SetForegroundWindow`，与主窗口一致 |
| 全屏应用中误触 | 新增前台进程黑名单 + 全屏检测（`GetForegroundWindow` + 比较窗口尺寸与屏幕尺寸） |
| 体积超标 | 预估新增 ~10 个源文件、~120KB，控制在 150KB 内 |

---

## 八、不做的事（明确边界）

1. **不做二级子轮盘**：v1 只做单层 4~8 扇区。
2. **不做轨迹手势识别**：不与 MouseInc 竞争。
3. **不做 OCR / 窗口管理等高级动作**：复用现有动作类型。
4. **不做主题工作室**：轮盘外观跟随主窗口主题，不单独做轮盘主题编辑。
5. **不做手势软件自动检测**：方案 C（自动检测 MouseInc）不实现，靠用户手动选择触发方式。

---

## 九、验收标准

1. 默认 `Ctrl+右键拖动` 呼出 8 扇区轮盘，松手触发对应动作。
2. 纯右键拖动不呼出轮盘，原生右键菜单正常弹出。
3. 前台为 `notepad.exe` 时显示记事本专属轮盘，其他程序显示全局轮盘。
4. 拖出取消半径外松手不触发任何动作。
5. 设置界面可切换触发方式、编辑扇区动作、配置 Per-App 映射。
6. `.\build.ps1` 与 `.\build.ps1 -Tests` 全部通过。
7. 与 MouseInc 同时运行时两者功能互不干扰。
