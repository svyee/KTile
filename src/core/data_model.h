// KTile - 核心数据模型
//
// 设计原则：
//  1. 磁贴尺寸是一等公民，从第一天就进入数据模型，避免后期迁移
//  2. 结构体一律提供构造函数（VS2017 不支持 C++20 指定初始化器）
//  3. 仅依赖标准库，不引入任何平台/JSON 细节（保持领域层纯净）
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// ================================================================ 常量

// 数据结构版本。变更数据结构时递增，并配套迁移逻辑。
inline constexpr std::uint32_t kCurrentSchemaVersion = 1;

// 网格容量约束（单位：格）
//
// 默认 8x6：这是启动器的初始窗口尺寸，能放下 8 列 6 行小格。
// 窗口尺寸只由这里的配置决定，**不随拖拽变化** —— 拖到边缘不会撑大
// 窗口，超出网格范围的落点会被夹回边界内。
// 用户只能在设置界面里改这两项。
inline constexpr std::uint16_t kDefaultGridColumns = 8;
inline constexpr std::uint16_t kDefaultGridRows = 6;
inline constexpr std::uint16_t kMinimumGridColumns = 3;
inline constexpr std::uint16_t kMinimumGridRows = 3;
inline constexpr std::uint16_t kMaximumGridColumns = 16;
inline constexpr std::uint16_t kMaximumGridRows = 16;

// ================================================================ 磁贴尺寸
//
// 这是 KTile 相对 HLaunch 的核心差异点。
// 小格基准 64x64 px，跨度以「小格」为单位：
//   Small = 1x1 (64x64)   Small 内不显示名称，悬停时显示
//   Wide  = 2x1 (128x64)  横向 2 格
//   Tall  = 1x2 (64x128)  纵向 2 格
//   Large = 2x2 (128x128) 占 4 格
//
enum class TileSize : std::uint8_t {
    Small = 0,   // 1 x 1 格，64 x 64 px
    Wide = 1,    // 2 x 1 格，128 x 64 px
    Tall = 2,    // 1 x 2 格，64 x 128 px
    Large = 3,   // 2 x 2 格，128 x 128 px
};

// 磁贴占据的列/行跨度（单位：小格）
inline std::uint16_t tileSpanColumns(TileSize size) noexcept {
    switch (size) {
        case TileSize::Small:  return 1;
        case TileSize::Wide:   return 2;
        case TileSize::Tall:   return 1;
        case TileSize::Large:  return 2;
    }
    return 1;
}

inline std::uint16_t tileSpanRows(TileSize size) noexcept {
    switch (size) {
        case TileSize::Small:  return 1;
        case TileSize::Wide:   return 1;
        case TileSize::Tall:   return 2;
        case TileSize::Large:  return 2;
    }
    return 1;
}

// 显示名（用于菜单/调试）
inline const wchar_t* tileSizeName(TileSize size) noexcept {
    switch (size) {
        case TileSize::Small:  return L"小 (64x64)";
        case TileSize::Wide:   return L"横向 (128x64)";
        case TileSize::Tall:   return L"纵向 (64x128)";
        case TileSize::Large:  return L"大 (128x128)";
    }
    return L"小 (64x64)";
}

// 从整数解析（用于配置读取），非法值回退 Small
inline TileSize tileSizeFromInt(int value) noexcept {
    switch (value) {
        case 1: return TileSize::Wide;
        case 2: return TileSize::Tall;
        case 3: return TileSize::Large;
        default: return TileSize::Small;
    }
}

inline int tileSizeToInt(TileSize size) noexcept {
    return static_cast<int>(size);
}

// ================================================================ 标题显示方式
//
// 每块磁贴可以单独决定「名字要不要一直显示」。
//
// 为什么不是全局一个开关就够：
//   64x64 的小磁贴标题会盖住图标，很多用户希望它平时干净、移到上面才看名字；
//   但同一个人摆在旁边的 128x128 大磁贴留白多，标题常驻反而更好用。
//   全局开关只能二选一，于是做成「全局定基调 + 每项可覆盖」。
enum class TileLabelMode : std::uint8_t {
    FollowGlobal = 0,   // 跟随设置里的「默认显示标题」（新项目的默认）
    Always = 1,         // 始终显示
    OnHover = 2,        // 鼠标移入才显示
};

// JSON 里用字符串而不是整数：将来增删枚举值时不会串位。
inline const char* tileLabelModeKey(TileLabelMode mode) noexcept {
    switch (mode) {
        case TileLabelMode::FollowGlobal: return "follow";
        case TileLabelMode::Always:       return "always";
        case TileLabelMode::OnHover:      return "hover";
    }
    return "follow";
}

inline TileLabelMode tileLabelModeFromKey(const std::string& key,
                                          bool* recognized = nullptr) noexcept {
    if (recognized) {
        *recognized = true;
    }
    if (key == "follow") return TileLabelMode::FollowGlobal;
    if (key == "always") return TileLabelMode::Always;
    if (key == "hover")  return TileLabelMode::OnHover;
    if (recognized) {
        *recognized = false;
    }
    return TileLabelMode::FollowGlobal;
}

// 下拉框里显示给用户的中文名
inline const wchar_t* tileLabelModeName(TileLabelMode mode) noexcept {
    switch (mode) {
        case TileLabelMode::FollowGlobal: return L"跟随全局设置";
        case TileLabelMode::Always:       return L"始终显示";
        case TileLabelMode::OnHover:      return L"移入时显示";
    }
    return L"跟随全局设置";
}

// 下拉框第 index 项（0..2）对应的枚举值
inline TileLabelMode tileLabelModeFromIndex(int index) noexcept {
    switch (index) {
        case 1: return TileLabelMode::Always;
        case 2: return TileLabelMode::OnHover;
        default: return TileLabelMode::FollowGlobal;
    }
}

inline int tileLabelModeToIndex(TileLabelMode mode) noexcept {
    return static_cast<int>(mode);
}

// ================================================================ 项目类型

enum class ItemType : std::uint8_t {
    Application = 0,
    File = 1,
    Folder = 2,
    Url = 3,
};

inline const char* itemTypeKey(ItemType type) noexcept {
    switch (type) {
        case ItemType::Application: return "application";
        case ItemType::File:        return "file";
        case ItemType::Folder:      return "folder";
        case ItemType::Url:         return "url";
    }
    return "application";
}

inline ItemType itemTypeFromKey(const std::string& key, bool* recognized = nullptr) noexcept {
    if (recognized) {
        *recognized = true;
    }
    if (key == "application") return ItemType::Application;
    if (key == "file")        return ItemType::File;
    if (key == "folder")      return ItemType::Folder;
    if (key == "url")         return ItemType::Url;
    if (recognized) {
        *recognized = false;
    }
    return ItemType::Application;
}

// ================================================================ 启动项目

struct LaunchItem {
    // 稳定唯一标识（UUID 字符串）。跨会话保持不变。
    std::string id;

    ItemType type = ItemType::Application;

    // 显示名。为空时由 target 推导。
    std::string name;

    // 目标：可执行文件路径 / 文件路径 / 文件夹路径 / URL
    std::string target;

    // 启动参数
    std::vector<std::string> arguments;

    // 工作目录（可空）
    std::optional<std::string> workingDirectory;

    // 自定义图标路径（可空，为空时从 target 提取）
    std::optional<std::string> icon;

    // 是否以管理员身份运行
    bool runAsAdministrator = false;

    // ---- 磁贴相关 ----

    // 磁贴尺寸（大中小混排的核心字段）
    TileSize tileSize = TileSize::Small;

    // 磁贴自定义背景色（ARGB，可空表示使用主题色）
    std::optional<std::uint32_t> tileColor;

    // 标题显示方式（每项覆盖全局设置，见 TileLabelMode）
    TileLabelMode labelMode = TileLabelMode::FollowGlobal;

    // 网格坐标。使用行/列定位，比线性 slot 更适合二维跨度布局。
    // 为空表示尚未分配位置，由布局引擎自动安置。
    std::optional<std::uint16_t> gridColumn;
    std::optional<std::uint16_t> gridRow;

    // ---- 统计 ----

    std::uint64_t launchCount = 0;
    std::optional<std::string> lastLaunchedAt;

    // ---- 构造函数（替代 C++20 指定初始化器）----
    LaunchItem() = default;

    LaunchItem(std::string id_,
               ItemType type_,
               std::string name_,
               std::string target_)
        : id(std::move(id_)),
          type(type_),
          name(std::move(name_)),
          target(std::move(target_)) {}

    // 是否有显式网格位置
    bool hasGridPosition() const noexcept {
        return gridColumn.has_value() && gridRow.has_value();
    }

    void clearGridPosition() noexcept {
        gridColumn.reset();
        gridRow.reset();
    }

    void setGridPosition(std::uint16_t column, std::uint16_t row) noexcept {
        gridColumn = column;
        gridRow = row;
    }
};

// ================================================================ 分类页
//
// 对应 HLaunch 的 Tab。KTile 保留此概念以便分组管理。

struct Tab {
    std::string id;
    std::string name;
    std::vector<LaunchItem> items;

    Tab() = default;

    Tab(std::string id_, std::string name_) : id(std::move(id_)), name(std::move(name_)) {}
};

// ================================================================ 项目文档（items.json）

struct ItemsDocument {
    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    std::vector<Tab> tabs;

    // 查找 Tab 索引，未找到返回 tabs.size()
    std::size_t findTabIndex(const std::string& tabId) const noexcept {
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].id == tabId) {
                return i;
            }
        }
        return tabs.size();
    }

    // 统计所有 Tab 的磁贴总数
    std::size_t totalItemCount() const noexcept {
        std::size_t count = 0;
        for (const auto& tab : tabs) {
            count += tab.items.size();
        }
        return count;
    }

    // 确保至少存在一个 Tab（首次运行或数据损坏时）
    Tab& ensureDefaultTab() {
        if (tabs.empty()) {
            tabs.push_back(Tab("default", "常用"));
        }
        return tabs.front();
    }
};

// ================================================================ 外观配置

// 主窗口每次唤出时出现在哪里。
//
// 默认「跟随鼠标」：启动器的核心用法是「热键唤出 → 立刻点/打字」，
// 窗口出现在手边比固定在屏幕中央少一次鼠标长距离移动。
enum class WindowPlacement : std::uint8_t {
    Mouse = 0,        // 鼠标所在位置（默认）
    LeftBottom = 1,   // 工作区左下角
    RightBottom = 2,  // 工作区右下角
    LeftTop = 3,      // 工作区左上角
    RightTop = 4,     // 工作区右上角
    Last = 5,         // 上一次拖动后的位置
    Center = 6,       // 屏幕（工作区）中央
};

inline const char* windowPlacementKey(WindowPlacement placement) noexcept {
    switch (placement) {
        case WindowPlacement::Mouse:       return "mouse";
        case WindowPlacement::LeftBottom:  return "leftBottom";
        case WindowPlacement::RightBottom: return "rightBottom";
        case WindowPlacement::LeftTop:     return "leftTop";
        case WindowPlacement::RightTop:    return "rightTop";
        case WindowPlacement::Last:        return "last";
        case WindowPlacement::Center:      return "center";
    }
    return "mouse";
}

inline WindowPlacement windowPlacementFromKey(
    const std::string& key) noexcept {
    if (key == "leftBottom")  return WindowPlacement::LeftBottom;
    if (key == "rightBottom") return WindowPlacement::RightBottom;
    if (key == "leftTop")     return WindowPlacement::LeftTop;
    if (key == "rightTop")    return WindowPlacement::RightTop;
    if (key == "last")        return WindowPlacement::Last;
    if (key == "center")      return WindowPlacement::Center;
    return WindowPlacement::Mouse;
}

struct AppearanceConfig {
    // 网格格数（以「小格」为单位）
    std::uint16_t gridColumns = kDefaultGridColumns;
    std::uint16_t gridRows = kDefaultGridRows;

    // 新建项目的默认磁贴尺寸
    TileSize defaultTileSize = TileSize::Small;

    // 窗口出现位置
    WindowPlacement windowPlacement = WindowPlacement::Mouse;

    // 「上一次的位置」用的窗口左上角（物理像素，屏幕坐标）。
    // valid = false 表示从未拖动过，此时该策略回落为「跟随鼠标」。
    std::int32_t lastWindowX = 0;
    std::int32_t lastWindowY = 0;
    bool lastWindowPositionValid = false;

    // 当前皮肤（<data>\themes\<名称>.json 的显示名）。
    // 空 = 用内置默认配色；文件被删时运行时回落默认值，不改写此名
    //（用户恢复文件后选择仍然有效）。
    std::string themeName;

    // 背景图（相对 data 目录的存储值，如 "themes\backgrounds\xx.jpg"；
    // 空 = 无背景图）。与 item.icon 同一约定：相对路径保证绿色可搬。
    std::string backgroundImage;

    // 是否显示磁贴文字标签
    bool showTileLabels = true;

    // 标题是否「默认显示」。
    //
    // 语义（与 showTileLabels 是两层开关）：
    //   showTileLabels = false          → 一律不显示标题（总闸）
    //   true  + alwaysShowTileLabels    → 所有尺寸的标题常驻显示
    //   true  + !alwaysShowTileLabels   → 只有鼠标移入的磁贴才浮出标题
    //
    // 之所以与总闸分开：一个磁贴只有 64x64 时标题会盖住图标，
    // 「平时干净、悬停才看名字」是很常见的偏好；但大磁贴留白多，
    // 标题常驻更实用。给用户一个开关，而不是替他决定。
    bool alwaysShowTileLabels = true;

    // 标签栏位置：false = 顶部（标题带下方，默认）；true = 底部（窗口下缘）。
    //
    // 窗口总高度公式不随此开关变化 —— 底部模式下标签条占据的高度
    // 从网格上方平移到网格下方，磁贴可视行数不变，切换位置不会
    // 引起窗口尺寸跳动。
    bool tabBarAtBottom = false;

    // 钉子（锁定窗口）。
    //
    // 语义：钉住时「点击窗口外部不隐藏」，方便把启动器当常驻面板用；
    // 松开时恢复「点击外部即隐藏」的轻量弹出行为。
    // 之所以放进 appearance 而不是 activation：它是窗口的显示行为，
    // 跟着用户对窗口外观的偏好走，且需要跨会话记住。
    bool pinned = false;

    // 主题强调色（ARGB）。空表示跟随系统。
    std::optional<std::uint32_t> accentColor;

    // ---- 不透明度 ----
    //
    // windowOpacityPercent：主窗口整体不透明度，50~100，100 = 完全不透明。
    //   走分层窗口（WS_EX_LAYERED）由系统统一合成 —— 窗口内容本身仍按
    //   不透明绘制，因此与「每帧 Clear 到不透明底」的防残影铁律不冲突。
    //   50 是下限：再低文字就没法读了，用户真的想要「几乎全透」应当去
    //   用别的工具，而不是让启动器变成一块看不见的玻璃。
    //
    // tileOpacityPercent：磁贴背景不透明度，0~100。
    //   -1 = 跟随皮肤（用皮肤 tileFill 自带的 alpha），这是默认值。
    //   为什么默认跟随而不是固定一个数字：不同皮肤对磁贴底的浓淡有自己
    //   的设计（「晨光」是 24% 白，深空是 24% 黑），固定值会让换皮肤后的
    //   观感走形。用户一旦拖动过滑块，就按他的值来，不再跟皮肤。
    std::int32_t windowOpacityPercent = 100;
    std::int32_t tileOpacityPercent = -1;

    // 上次显示的是哪一个标签页（存 Tab 的 id，不是下标）。
    //
    // 为什么存 id 不存下标：页可以被删除、左右移动，下标一变动
    // 「上次在第 3 页」就会指向另一页；id 是稳定的，找不到就退回第一页。
    // 空串 = 还没记录过（首次运行）。
    std::string activeTabId;
};

// ================================================================ 激活配置

enum class HotkeyModifier : std::uint8_t {
    Alt = 0,
    Control = 1,
    Shift = 2,
    Win = 3,
};

// 唤醒方式：除了传统「修饰键 + 主键」，还支持两种手势。
//
// 手势需要低级键盘钩子（RegisterHotKey 无法表达），因此只在选中时
// 才安装钩子 —— 默认的 Alt+Space/Ctrl+Space 仍是零开销的注册热键。
enum class HotkeyGesture : std::uint8_t {
    None = 0,        // 用 modifiers + key（RegisterHotKey）
    DoubleCtrl = 1,  // 双击 Ctrl
    WinKey = 2,      // 单击 Windows 键（Ctrl+Win 打开开始菜单）
};

inline const char* hotkeyGestureKey(HotkeyGesture gesture) noexcept {
    switch (gesture) {
        case HotkeyGesture::None:       return "none";
        case HotkeyGesture::DoubleCtrl: return "doubleCtrl";
        case HotkeyGesture::WinKey:     return "winKey";
    }
    return "none";
}

inline HotkeyGesture hotkeyGestureFromKey(const std::string& key) noexcept {
    if (key == "doubleCtrl") return HotkeyGesture::DoubleCtrl;
    if (key == "winKey")     return HotkeyGesture::WinKey;
    return HotkeyGesture::None;
}

struct HotkeyConfig {
    bool enabled = true;
    std::vector<HotkeyModifier> modifiers{HotkeyModifier::Alt};
    // 主键名（如 "Space", "F1", "A"）
    std::string key = "Space";
    // 手势优先级高于 modifiers + key：非 None 时忽略后两者
    HotkeyGesture gesture = HotkeyGesture::None;
};

// 设置界面提供的四种预设。
//
// 之所以用「预设」而不是让用户自由录制组合键：录制要处理修饰键、
// 给用户反馈、冲突检测，收益却很低 —— 这四个覆盖了绝大多数诉求，
// 也正好把两种手势和两种注册热键放在同一排选项里。
enum class HotkeyPreset : std::uint8_t {
    AltSpace = 0,
    CtrlSpace = 1,
    DoubleCtrl = 2,
    WinKey = 3,
};

inline HotkeyConfig hotkeyConfigForPreset(HotkeyPreset preset) {
    HotkeyConfig config;
    switch (preset) {
        case HotkeyPreset::AltSpace:
            config.modifiers = {HotkeyModifier::Alt};
            config.key = "Space";
            config.gesture = HotkeyGesture::None;
            break;
        case HotkeyPreset::CtrlSpace:
            config.modifiers = {HotkeyModifier::Control};
            config.key = "Space";
            config.gesture = HotkeyGesture::None;
            break;
        case HotkeyPreset::DoubleCtrl:
            config.gesture = HotkeyGesture::DoubleCtrl;
            break;
        case HotkeyPreset::WinKey:
            config.gesture = HotkeyGesture::WinKey;
            break;
    }
    return config;
}

inline HotkeyPreset hotkeyPresetOf(const HotkeyConfig& config) noexcept {
    if (config.gesture == HotkeyGesture::DoubleCtrl) {
        return HotkeyPreset::DoubleCtrl;
    }
    if (config.gesture == HotkeyGesture::WinKey) {
        return HotkeyPreset::WinKey;
    }
    const bool ctrlOnly = config.modifiers.size() == 1 &&
        config.modifiers.front() == HotkeyModifier::Control;
    if (ctrlOnly && config.key == "Space") {
        return HotkeyPreset::CtrlSpace;
    }
    // 其余（含老配置的 Alt+Shift+F9 之类）统一回落到 Alt+Space 预设显示
    return HotkeyPreset::AltSpace;
}

struct ActivationConfig {
    HotkeyConfig hotkey;

    // 是否开机自启
    bool startWithWindows = false;

    // 是否启用屏幕边缘唤起（v2 功能，先留字段）
    bool screenEdgeEnabled = false;
};

// ================================================================ 应用总配置（config.json）

struct ApplicationConfig {
    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    AppearanceConfig appearance;
    ActivationConfig activation;

    // 是否启用诊断日志
    bool loggingEnabled = true;
};

// ================================================================ 校验常量

// 名称/ID 长度上限
inline constexpr std::size_t kMaximumNameLength = 256;
inline constexpr std::size_t kMaximumTargetLength = 4096;
inline constexpr std::size_t kMaximumIdLength = 64;

}  // namespace core
}  // namespace ktile
