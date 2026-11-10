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

// 一份轮盘（用户配置里只有一份通用轮盘；插件里每份对应一个目录）
struct RadialMenu {
    std::string id;
    std::string name;
    std::uint8_t sectorCount = 8;              // 4 / 6 / 8
    std::vector<RadialAction> actions;         // 固定 sectorCount 个，空槽位留空 action
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

    // 抑制键：按住其中任意一个时轮盘不呼出、鼠标事件原样放行
    std::vector<HotkeyModifier> suppressWhileHeld;
};

// 轮盘功能的总配置
//
// 只有一份「通用轮盘」：按应用差异化交给 slugin 插件目录（见 4.6），
// 用户配置里不再有方案列表与按应用白名单。
struct RadialConfig {
    std::uint32_t schemaVersion = kRadialSchemaVersion;   // 当前为 3
    RadialTriggerConfig trigger;
    RadialMenu menu;
    std::vector<std::string> blacklist;   // 命中即不弹轮盘（只比进程名）
};

}  // namespace ktile::core
```

### 3.2 持久化

- 独立文件 `data/radial.json`，带 `schemaVersion`（当前为 **3**）。
- 版本线独立：用 `core::kRadialSchemaVersion` 而不是 `data_model.h` 的
  `kCurrentSchemaVersion`——两者结构毫无关系，共用常量会让给轮盘加字段时
  连带把磁贴配置的版本号推上去。
- 复用 `infrastructure/json` 的 `JsonParser` / `JsonWriter`。
- 损坏时把原文件改名备份为 `.corrupt` 并返回默认值（与 `config.json` 同一套
  容错规则）。
- 原子写：先写 `.tmp` 再替换。
- **没有迁移函数**（有意为之）：字段缺失走默认值、认不出的枚举值回落到安全项、
  越界值在加载时夹取，老文件本来就能读。读成功后统一把 `schemaVersion` 盖成
  当前值——否则下次保存又会把旧值写回去，以后真要做破坏性变更时就没有判断
  依据了。
- 加载时的清洗与夹取规则（防止手改的离谱值流进 UI 和窗口）：
  - `activationThresholdPx` < 1 → 1（0 等于「按下即呼出」，会把触发键彻底吃掉）
  - `cancelRadiusPx` 夹到 `[kMinCancelRadiusDip, kMaxCancelRadiusDip]`（142~600 DIP）
  - `sectorCount` 夹到 4~8，`actions` 补齐到 `sectorCount` 个
  - `modifier` 键缺失 → 回落到默认的 `Ctrl`（不是 `None`：`None` 是纯按键拖动，
    会把触发键整个吞掉，手写配置忘了这一项的用户多半不想这样）
  - `suppressWhileHeld` 里认不出的键名跳过、重复项去重、与触发修饰键撞车的
    剔除（撞车等于永远不触发）；剔除单项而不是整份打回默认——用户的扇区配置
    比抑制键值钱
  - `blacklist` 逐项削首尾空白、去掉空项、按大小写不敏感去重

**版本迁移（2 → 3）**：老文件的 `menus` 数组里可能有多份方案，优先取 `id` 为
`default` 的那份当通用轮盘；`profiles`（按应用白名单）**整段丢弃**——白名单
的语义是「这些应用用别的轮盘」，转成黑名单会变成「这些应用不弹轮盘」，那是
反的。按应用差异化改由 `slugin` 插件承担。

**默认数据**：首次运行时写入一个 8 扇区的通用菜单（`id` = `default`，扇区
0 / 2 / 4 / 6 预填复制、粘贴、剪切、撤销，其余留空），黑名单为空。空槽位只画
一层很淡的填充 + 细轮廓（不画阴影），让环的骨架看得见但不抢戏。

**单位约定**：`activationThresholdPx` 是**物理像素**（钩子里直接量鼠标位移），
`cancelRadiusPx` 是 **DIP**（卡片布局本身就是 DIP；按物理像素算的话，150% 缩放
的机器上「取消半径 160」会落进卡片内部，怎么拖都会取消）。

---

## 四、分层架构与新增模块

遵循 AGENTS.md 的依赖方向规则，只向下依赖。

### 4.1 文件清单（全部已落地）

| 层 | 文件 | 职责 |
|---|---|---|
| core | `radial_menu.h` | 数据模型 + `sectorFromAngle` / `hitTestSector` 声明（纯函数，可单测） |
| core | `radial_menu.cpp` | 角度 → 扇区索引、命中测试、配置校验等纯逻辑 |
| core | `radial_plugin.h` / `.cpp` | 轮盘插件模型：进程匹配（priority + id 字典序）、插件校验 |
| infrastructure | `radial_plugin_store.h` / `.cpp` | 扫描 `slugin/<id>/radial.json`、解析、图标路径解析成绝对路径 |
| platform/win32 | `foreground_process.h` / `.cpp` | 获取前台窗口的进程名 |
| platform/win32 | `radial_mouse_hook.h` / `.cpp` | `WH_MOUSE_LL` 钩子 + 手势状态机 |
| platform/win32 | `key_injector.h` / `.cpp` | `SendInput` 注入任意修饰键+主键组合 |
| infrastructure/json | `radial_store.h` / `.cpp` | `radial.json` 的读写、越界夹取、损坏备份 |
| ui | `radial_menu_window.h` / `.cpp` | 轮盘窗口（D2D 卡片绘制 + 标签自适应排版 + 命中测试 + 出现动画） |
| ui | `radial_editor_dialog.h` / `.cpp` | 轮盘编辑器（扇区动作 + 进程黑名单两页） |
| ui | `settings_window.h` / `.cpp` | 设置第 4 页「轮盘」：启用、触发方式、阈值、取消半径、抑制键；第 5 页「轮盘插件」：只读清单 + 打开 JSON / 目录 + 重新扫描 |
| app | `application.h` / `.cpp` | 组装：钩子 → 匹配插件 → 通用轮盘 → 显示窗口 → 分发动作 |
| tests | `test_radial_menu.cpp` | core 层几何、校验、黑名单、抑制键、插件匹配单测 |
| tests | `test_persistence.cpp` | 轮盘配置往返、空槽位不错位、越界夹取、版本号盖章、插件扫描与图标路径 |

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
  2. 先在 `slugin` 插件里按前台进程名匹配（大小写不敏感，多命中看 priority，
     并列按 id 字典序），没命中才用通用轮盘 `RadialConfig::menu`。
     插件优先是有意的：插件是「这个应用该有哪些动作」的专门答案，通用轮盘是
     「所有应用都能凑合用」的兜底；反过来插件永远轮不到。
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
| 外观 | 4~8 个扇形（`ID2D1PathGeometry` + `AddArc` 拼圆盘） | **卡片环**：8 个 62×40 DIP 轴对齐圆角卡片挂在半径 97 的圆上，中心留空圆。卡片早期是沿径向旋转的"花瓣"，1.2.1 起改成正放 | 扇形切片的文字要跟着旋转才放得下，长中文名会歪；卡片式标签可正向排布，且空槽位的"骨架"更清楚 |
| 卡片朝向 | 沿径向旋转的"花瓣" | **轴对齐**（不再随角度旋转） | 卡片里放一行水平文字，而 62×40 的矩形转过 45° 后容得下的居中水平文字只剩三十几 DIP——四个汉字（40 DIP）就从斜边探出去，看着像"字跑到按钮外面"。放大到 62×62 才装得下，但窗口半径会被撑爆。 |
| 标签排版 | 固定 10 DIP、不换行 | 放得下用 10 DIP；放不下按比例缩（下限 8 DIP）；缩到下限还超宽就**截断加省略号**；最后还有 `D2D1_DRAW_TEXT_OPTIONS_CLIP` 兜底 | 标签长度由用户 / 插件作者决定（"新建文件夹" 5 个字正好 50 DIP，比旧卡片还宽）。硬裁剪会在字中间切一刀，看起来像渲染坏了；缩到 8 DIP 以下又等于把字变成灰点。 |
| 扇区数 | 4~8（连续可选） | **4 / 6 / 8** 三档 | 5 / 7 扇区的边界角是无限小数，命中测试的边界判定容易在两个扇区之间抖 |
| 扇区命中 | 极坐标角度 + 半径 | 同（`core::hitTestSector`），但外半径来自 `cancelRadiusPx`（DIP），下限夹到 142 | 滑杆原来不生效；单位必须是 DIP，见 3.2 |
| 设置入口 | 设置页内联「方案列表 + 扇区编辑器 + Per-App」 | 设置页只放触发配置；扇区动作与黑名单放进**独立对话框**（`radial_editor_dialog`）；按应用改由第 5 页「轮盘插件」只读展示 `slugin/` 目录 | 700px 单列设置表单塞不下列表 + 表单 + 映射三重编辑，纵向会溢出 |
| 结果交接 | `SettingsResult` 增加若干轮盘字段 | 交**整份 `RadialConfig`** | 扇区动作是数组，字段级交接表达不了"清空某个扇区" |
| 抬起兜底 | 依赖钩子可靠 | 轮盘可见期间挂 **250ms 看门狗**，按三种判据收起：钩子已失效 / 钩子回到 Idle 但窗口还亮着 / 可见超过 10 秒 | 低级钩子回调会被系统静默跳过，而轮盘窗口 `WS_EX_NOACTIVATE` 没有焦点、收不到 Esc，一旦漏掉抬起就永久挂屏。**不能用 `GetAsyncKeyState` 问「键还按着吗」**：我们吞掉了按下事件，鼠标键的异步状态不会更新，整个拖动期间它都回答「已松开」（见下方 4.5） |
| 侧键区分 | "捕获侧键的按下事件" | 按 `HIWORD(MSLLHOOKSTRUCT.mouseData)` 判定 XBUTTON1 / XBUTTON2，抬起同样校验 | `WM_XBUTTON*` 把两个侧键并成同一消息，只比消息类型会让侧键 1 也触发侧键 2 的轮盘 |
| 矢量图标 | 未提及 | 快捷键（复制 / 粘贴 / 剪切 / 撤销 / 保存）与内置命令走内嵌 Lucide 形状，按**弧长加权质心**居中 | 从 exe 提不到这类动作的图标；按 24×24 外框几何中心摆放时，剪刀、撤销箭头这类不对称图形看起来偏 |
| 缩放 | 未提及 | 整体比初稿小约 10%（窗口半径 158 → 142 DIP） | 142 半径下相邻卡片之间留 6~10 DIP 缝隙，看出是"一圈"而不是一团 |
| schemaVersion | 初始为 1 | 当前为 **3**（独立版本线 `kRadialSchemaVersion`），读成功即盖成当前值，没有迁移函数 | 容错策略（默认值 + 回落 + 夹取）已经能读老文件，再写迁移函数是重复劳动；但版本号必须跟着升，否则以后真做破坏性变更时没有判断依据 |

### 4.5 一条硬约束：吞掉事件的钩子问不出物理按键状态

`WH_MOUSE_LL` 回调里把「按下」事件吞掉之后：

| 查询方式 | 结果 | 为什么 |
|---|---|---|
| `GetAsyncKeyState(VK_RBUTTON)` | 一直回答「未按下」 | 鼠标键的异步状态在事件**通过钩子链、进入目标队列**时才更新；被吞的事件从未进入队列 |
| `GetKeyState(VK_RBUTTON)` | 同上（而且本线程没有该键的消息） | 读的是线程消息队列里的状态 |
| 钩子自己的状态机 | 正确 | 它按事件流推进，不依赖系统状态 |

所以「用户还按着吗」只能由钩子自己回答（`RadialMouseHook::isGestureActive()`），
或者靠**时间上限**兜底。1.2.0 的看门狗就是栽在这一条上：它每 120ms 调一次
`GetAsyncKeyState`，答案永远是「松开了」，于是轮盘刚出现就被强制收起，用户
根本来不及选。这个教训值得记住：**凡是吞事件的钩子，都不要用系统状态查询
去交叉验证它**。

### 4.6 轮盘插件（`slugin/`）

#### 为什么把「按应用」改成插件

按应用换轮盘本质上是一份「进程名 → 扇区配置」的映射。把它做进设置界面，就等于
要求主程序认识每个应用该有哪些动作；改成目录 + JSON 之后，加一个应用就是加一个
目录，不用改代码、不用重新编译、不用发版。

**顺带纠正一个预期**：这不会让 exe 变小多少。内嵌的 Lucide 图标（`lucide_icons.h`）
只有约 8 KB，1.25 MB 的产物绝大部分是 `/MT` 静态 CRT 与 D2D/DWrite/WIC 胶水代码。
真正的收益是「扩展不用重新编译」和「改图标不用改代码」。

#### 目录与清单

```
<exe 同级>/slugin/<id>/radial.json     # 清单，目录名即插件 id
<exe 同级>/slugin/<id>/icons/*.svg     # 按钮图标，也可 png / ico
```

```json
{
  "schemaVersion": 1,
  "name": "文件资源管理器",
  "processes": ["explorer.exe"],
  "priority": 0,
  "menu": { "name": "资源管理器", "sectorCount": 8, "actions": [ ... ] }
}
```

- `schemaVersion` 比本程序新 → **整份不加载**并给出原因（字段语义可能已经变了，
  猜着读比不读更糟）。老插件缺字段则走默认值，照常加载。
- `menu` 与用户配置里的 `menu` 共用同一个解析函数（`parseRadialMenuJson`），
  字段格式完全一致——少一份定义就少一处走样。
- `processes` 只写进程名（`explorer.exe`），**不支持通配符**：`*app*.exe` 这种
  写法误伤面太大，用户很难预估哪个应用会被抢走。

#### 匹配规则

`core::findPluginForProcess(plugins, processName)`：

1. 只考虑 `valid` 的插件，进程名逐项比较（ASCII 大小写不敏感）；
2. 多个命中时 `priority` 大的赢；
3. 仍并列则按 id **字典序**取第一个——目录遍历顺序在不同文件系统上不一致，
   没有这条兜底，同一份配置在两台机器上可能显示不同轮盘。

没有命中返回 `nullptr`，调用方回落到通用轮盘。

#### 图标解析

清单里的 `icon` 相对**插件目录**解析，且在**加载期**就转成绝对路径。
放在加载期而不是渲染期，是因为渲染层只知道「icon 是个路径」：用户配置的图标
相对 `data/`、插件的相对插件目录，留相对路径就变成「图标全丢，但没人报错」。

渲染顺序（`RadialMenuWindow::paintCell`）：动作自带的 SVG → 可提取的位图 / 文件
图标 → 内置 Lucide 符号。自带图标排第一，因为插件作者放图标就是要它生效。

- SVG 沿用磁贴插件的约定：**24×24 用户单位**，可直接用 `path` / `rect` /
  `circle` / `line` / `polyline` / `polygon`；带 `stroke="#RRGGBB"` 的图层保留
  自身颜色，只继承当前透明度（高亮反色时不跟着跳色）。
- 位图（png / ico / jpg…）走 `loadIconFromFile` 的 WIC 解码。快捷键类动作此前
  一律 `return nullptr`（"没有文件图标"），现在只要给了自定义位图就用它——插件
  里几乎全是快捷键动作，否则「图标外置」这条诉求等于白提。
- 缓存键带文件修改时间：换掉插件目录里的 SVG 后下一帧就生效，不必重启。

#### 坏插件只跳过自己

`discover()` 的容错边界：

| 情况 | 处理 |
|---|---|
| 目录没有 `radial.json` | 直接忽略（不是插件，例如别人的图标目录） |
| JSON 语法错 | 该插件 `valid=false`，`error` 写明原因，其余插件照常 |
| `processes` 为空 / 含空串 | 同上（空串会让「按进程匹配」命中所有人） |
| `sectorCount` 不在 4~8，或动作数与扇区数不一致 | 同上 |
| `schemaVersion` 比本程序新 | 同上 |

轮盘是最常用的入口，一个手滑的逗号不该让它整体消失。

#### 设置页为什么是只读

插件形态是「目录 + JSON + 图标」。做成表格编辑器就等于宣布「插件只能在界面里
配」，那样「加个目录就是加个应用」的意义就没了，还要为它维护一套自定义字段的
UI。所以第 5 页只做三件事：看清加载了哪些、看清某个为什么被跳过、
一键用系统默认编辑器打开那份 JSON（无文件关联时记事本兜底）。

#### 怎么验证

`--debug-radial-plugin=<id>` 让 `--debug-radial` 渲染**插件**的菜单而不是通用
轮盘，导出 `radial-shot.png`。没有这个开关，插件自带图标与扇区布局就只能在前台
应用里肉眼看，自动化无从验证。

#### 不做的事

- 不做表格编辑器（理由见上）。
- 不做目录热监听：`ReadDirectoryChangesW` 常驻线程换来的只是「省一次点重新扫描」，
  与「无输入时全静默」的取舍不符。
- 不让插件覆盖通用轮盘的触发方式 / 抑制键 / 黑名单：那三项属于「什么时候弹」，
  与「弹出来是什么」不是一回事，交给插件会让排查问题变成三方对账。

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
      - 「扇区动作」页：扇区数 4 / 6 / 8、扇区列表（扇区 / 类型 / 名称 / 目标）、
        右侧动作表单（6 种类型 + 名称 + 修饰键 + 主键 + 目标 + 选择…按钮）
      - 「黑名单」页（原「按应用」页改造）：进程名输入 + 列表，命中即不弹轮盘。
        按应用换轮盘的诉求改由 `slugin` 插件承担（见 4.6），界面不再维护方案列表
- [x] 一条不变量在对话框内部强制执行：`actions.size()` 恒等于 `sectorCount`

> 二次调整：1.2.0 落地时这里是「多方案 + 按应用白名单」，1.2.1 起收敛为
> 「一份通用轮盘 + 进程黑名单 + slugin 插件」，方案列表与白名单界面随之删除。
> 效果等价（按应用仍能显示不同轮盘），但配置从「主程序里的映射表」变成
> 「exe 同级的目录」，加应用不用重新编译。
- [x] `application.cpp`：设置返回后重装钩子（触发方式可能变了）、落盘、刷新托盘提示
- [x] `build.ps1`：`Get-AppSources` 加入 `radial_editor_dialog.cpp`

### 阶段 6.5：抑制键 + 黑名单 + slugin 插件（后续需求，1 天）

- [x] `RadialTriggerConfig::suppressWhileHeld`：按住 Ctrl / Shift / Alt / Win
      时整段放行（抑制键是**键盘**键，没被钩子吞过，`GetAsyncKeyState` 可信——
      与 4.5 那条硬约束不冲突）
- [x] 黑名单在**钩子按下时**判：等到窗口显示时再判，按下事件已经被吞掉，
      原生右键菜单弹不出来，用户会以为「这个应用里右键坏了」
- [x] 新增 `core/radial_plugin` + `infrastructure/radial_plugin_store`，
      `Application::onRadialShow` 改成「插件优先 → 通用轮盘兜底」
- [x] 设置第 5 页「轮盘插件」：只读清单 + 打开 JSON / 目录 + 重新扫描
- [x] 轮盘扇区支持自带 SVG / 位图图标（含快捷键类动作）
- [x] `--debug-radial-plugin=<id>`：导出插件轮盘截图，供自动化验证图标与布局
- [x] 示例插件 `slugin/explorer`

### 阶段 7：验证（1 天）

- [x] `.\build.ps1`：56 个源文件，0 告警
- [x] `.\build.ps1 -Tests`：12 个测试全部通过（`test_radial_menu` 几何 / 校验 /
      插件匹配用例，`test_persistence` 224 项检查，含插件扫描与图标路径解析）
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
- [x] 抬起事件丢失后轮盘永久挂屏（加看门狗定时器，按三种判据收起）
- [x] 看门狗误杀：第一版用 `GetAsyncKeyState` 判断触发键物理状态，被吞掉的按下
      事件永远不会更新异步键状态，于是每 120ms 判一次「已松开」、轮盘刚出现就消失
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
| 全屏应用中误触 | 进程黑名单（按进程名，命中即整段放行）；**不做全屏检测**——用户的取舍是「只按键抑制，不要猜」 |
| 插件写坏把轮盘整个搞挂 | 每个插件独立校验，坏的只跳过自己并在设置页写明原因；`schemaVersion` 更新的插件整份拒收 |
| 体积超标 | 实测新增 9 个源文件、约 55KB（1197 KB → 1252 KB），远低于 150KB 预算 |

---

## 八、不做的事（明确边界）

1. **不做二级子轮盘**：v1 只做单层 4~8 扇区。
2. **不做轨迹手势识别**：不与 MouseInc 竞争。
3. **不做 OCR / 窗口管理等高级动作**：复用现有动作类型。
4. **不做主题工作室**：轮盘外观跟随主窗口主题，不单独做轮盘主题编辑。
5. **不做手势软件自动检测**：方案 C（自动检测 MouseInc）不实现，靠用户手动选择触发方式。
6. **不做插件目录热监听 / 表格编辑器**：插件是外部文件，只做「扫描 + 只读展示 + 一键打开」，见 4.6。

---

## 九、验收标准

| # | 标准 | 状态 |
|---|---|---|
| 1 | 默认 `Ctrl+右键拖动` 呼出 8 扇区轮盘，松手触发对应动作 | 待人工真机确认 |
| 2 | 纯右键拖动不呼出轮盘，原生右键菜单正常弹出 | 待人工真机确认 |
| 3 | 前台进程命中插件时显示该插件的轮盘，否则显示通用轮盘 | ✅ 匹配有单测；`--debug-radial-plugin=explorer` 已导出带插件图标的截图；真机前台切换待确认 |
| 4 | 拖出取消半径外松手不触发任何动作 | ✅ 半径已接进命中测试；手感待人工调 |
| 5 | 设置界面可切换触发方式、编辑扇区动作与黑名单 | ✅ 已用脚本驱动点击验证落盘 |
| 6 | 「轮盘插件」页能列出插件、显示跳过原因、打开 JSON、重新扫描 | ✅ 截图验证（含一个故意写坏的插件） |
| 7 | `.\build.ps1` 与 `.\build.ps1 -Tests` 全部通过 | ✅ 12 个测试全通过（持久化 224 项检查）/ 0 告警 |
| 8 | 与 MouseInc 同时运行时两者功能互不干扰 | 待人工真机确认 |
