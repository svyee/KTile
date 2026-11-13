// KTile - 核心数据模型
//
// 仅依赖标准库，不引入平台/JSON 细节（保持领域层纯净）。
// 磁贴尺寸是一等公民；结构体一律提供构造函数（VS2017 不支持
// C++20 指定初始化器）。
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// --- 常量

// 数据结构版本，变更时递增并配套迁移。历次演进：
// v2 加插件磁贴（Plugin + pluginId + pluginParams）；v3 加
// activation.followLaunch；v4 让 followLaunch 条目带 itemId 与
// 参数/工作目录/管理员快照。都只增字段，旧文件缺失字段走默认值
// （followLaunch 为空表、老条目读成「手动添加」语义），不需要迁移
// 函数，也不改写原文件。
inline constexpr std::uint32_t kCurrentSchemaVersion = 4;

// 网格容量约束（单位：格）。默认 8x6 即启动器初始窗口尺寸；窗口尺寸
// 只由这里的配置决定，不随拖拽变化，越界落点被夹回边界内。
inline constexpr std::uint16_t kDefaultGridColumns = 8;
inline constexpr std::uint16_t kDefaultGridRows = 6;
inline constexpr std::uint16_t kMinimumGridColumns = 3;
inline constexpr std::uint16_t kMinimumGridRows = 3;
inline constexpr std::uint16_t kMaximumGridColumns = 16;
inline constexpr std::uint16_t kMaximumGridRows = 16;

// --- 磁贴尺寸（与 HLaunch 等分网格的核心差异点）
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

// 显示名的 key（菜单/下拉框用）。core 不允许依赖 infrastructure，
// 显示名由 UI 层查语言文件；尺寸标注也写进语言文件，换语言时
// 连单位排版都能跟着变。
inline const char* tileSizeLabelKey(TileSize size) noexcept {
    switch (size) {
        case TileSize::Small:  return "size.small";
        case TileSize::Wide:   return "size.wide";
        case TileSize::Tall:   return "size.tall";
        case TileSize::Large:  return "size.large";
    }
    return "size.small";
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

// --- 标题显示方式
//
// 全局定基调 + 每项可覆盖：64x64 小磁贴的标题会盖图标，悬停显示更
// 干净；128x128 大磁贴留白多，常驻标题更实用。
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

// 下拉框里显示给用户的文案 key（由 UI 层查语言文件）
inline const char* tileLabelModeLabelKey(TileLabelMode mode) noexcept {
    switch (mode) {
        case TileLabelMode::FollowGlobal: return "labelmode.follow";
        case TileLabelMode::Always:       return "labelmode.always";
        case TileLabelMode::OnHover:      return "labelmode.hover";
    }
    return "labelmode.follow";
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

// --- 项目类型

enum class ItemType : std::uint8_t {
    Application = 0,
    File = 1,
    Folder = 2,
    Url = 3,
    // 插件磁贴：数据由 data/plugins/<id>/plugin.json 声明，
    // 磁贴本身只存插件 id 与少量参数（见 LaunchItem::pluginId）。
    Plugin = 4,
};

inline const char* itemTypeKey(ItemType type) noexcept {
    switch (type) {
        case ItemType::Application: return "application";
        case ItemType::File:        return "file";
        case ItemType::Folder:      return "folder";
        case ItemType::Url:         return "url";
        case ItemType::Plugin:      return "plugin";
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
    if (key == "plugin")      return ItemType::Plugin;
    if (recognized) {
        *recognized = false;
    }
    return ItemType::Application;
}

// --- 启动项目

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

    // ---- 插件磁贴（type == Plugin 时有效）----
    //
    // 只存插件 id 与少量参数，不存 manifest 内容：插件升级后磁贴
    // 跟着新 manifest 走，不需要迁移 items.json。参数是扁平字符串
    // 键值对，覆盖天气城市、命令行开关等场景。
    std::optional<std::string> pluginId;
    std::vector<std::pair<std::string, std::string>> pluginParams;

    // 取插件参数（不存在返回空串）
    std::string pluginParam(const std::string& key) const noexcept {
        for (const auto& entry : pluginParams) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return std::string();
    }

    void setPluginParam(const std::string& key, const std::string& value) {
        for (auto& entry : pluginParams) {
            if (entry.first == key) {
                entry.second = value;
                return;
            }
        }
        pluginParams.emplace_back(key, value);
    }

    bool runAsAdministrator = false;

    // ---- 磁贴相关 ----

    TileSize tileSize = TileSize::Small;

    // 磁贴自定义背景色（ARGB，可空表示使用主题色）
    std::optional<std::uint32_t> tileColor;

    // 标题显示方式（每项覆盖全局设置，见 TileLabelMode）
    TileLabelMode labelMode = TileLabelMode::FollowGlobal;

    // 图标相对磁贴内框的缩放百分比（50~100）。0 = 跟随全局
    // appearance.iconScalePercent（新项目默认），100% = 内框铺满。
    // 只影响系统提取的方框图标；用户导入的铺满整图不受约束。
    int iconScalePercent = 0;

    // 网格坐标（行/列），空 = 尚未分配，由布局引擎自动安置。
    std::optional<std::uint16_t> gridColumn;
    std::optional<std::uint16_t> gridRow;

    // ---- 统计 ----

    std::uint64_t launchCount = 0;
    std::optional<std::string> lastLaunchedAt;

    // ---- 构造函数（VS2017 不支持 C++20 指定初始化器）----
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

// --- 分类页（对应 HLaunch 的 Tab，保留用于分组管理）

struct Tab {
    std::string id;
    std::string name;
    std::vector<LaunchItem> items;

    Tab() = default;

    Tab(std::string id_, std::string name_) : id(std::move(id_)), name(std::move(name_)) {}
};

// --- 项目文档（items.json）

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

// --- 外观配置

// 主窗口每次唤出时出现的位置。默认「跟随鼠标」：核心用法是热键唤出后
// 立刻点/打字，窗口出现在手边比固定屏幕中央少一次鼠标移动。
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

    // 当前皮肤（<data>\themes\<名称>.json 的显示名）。空 = 内置默认配色；
    // 文件被删时运行时回落默认值但不回写此名，用户恢复文件后选择仍有效。
    std::string themeName;

    // 背景图（相对 data 目录的存储值，如 "themes\backgrounds\xx.jpg"；
    // 空 = 无背景图）。与 item.icon 同一约定：相对路径保证绿色可搬。
    std::string backgroundImage;

    // 是否显示磁贴文字标签
    bool showTileLabels = true;

    // 标题是否「默认显示」，与 showTileLabels 是两层开关：
    // showTileLabels=false 一律不显示；true+本项 所有尺寸标题常驻；
    // true+关 仅鼠标移入的磁贴浮出标题。小磁贴常驻盖图标、大磁贴
    // 常驻更实用，交给用户选。
    bool alwaysShowTileLabels = true;

    // 标签栏位置：false = 顶部（默认），true = 底部（窗口下缘）。
    // 窗口总高度公式不随此开关变化，磁贴可视行数不变，切换不引起
    // 窗口尺寸跳动。
    bool tabBarAtBottom = false;

    // 钉住时点击窗口外部不隐藏（当常驻面板用），松开恢复点外即隐藏。
    // 放进 appearance 而非 activation：它是窗口显示行为且需跨会话记住。
    bool pinned = false;

    // 主题强调色（ARGB）。空表示跟随系统。
    std::optional<std::uint32_t> accentColor;

    // 不透明度。
    // windowOpacityPercent：主窗口整体不透明度，50~100，走分层窗口
    //   （WS_EX_LAYERED）系统合成，与每帧 Clear 到不透明底的防残影
    //   规则不冲突；50 是文字可读性下限。
    // tileOpacityPercent：磁贴背景不透明度，0~100；-1 = 跟随皮肤
    //   tileFill 自带的 alpha（默认）。不同皮肤浓淡设计不同，固定值
    //   会让换肤观感走形；用户拖过滑块后按用户的值，不再跟皮肤。
    std::int32_t windowOpacityPercent = 100;
    std::int32_t tileOpacityPercent = -1;

    // 所有磁贴的默认背景色（ARGB）。空 = 跟随皮肤 tileFill。
    // 优先级：单项 LaunchItem.tileColor > 这里的全局默认色 > 皮肤。
    // 单独做全局字段是因为常需要一次性换掉所有格子底色。设置全局色后
    // 不再套用 tileOpacityPercent，套到用户挑的实色上会让颜色变脏
    // （与单项色规则一致）。
    std::optional<std::uint32_t> defaultTileColor;

    // 图标统一缩放百分比，50~100（默认 90），100% = 图标铺满内框。
    // 单项可在编辑对话框覆盖（LaunchItem.iconScalePercent，0 = 跟随这里）。
    // 只改格子里的图标，磁贴布局与窗口尺寸不受影响。
    std::int32_t iconScalePercent = 90;

    // 上次显示的标签页，存 Tab 的 id 而非下标：页可删可移，下标会指向
    // 别的页，id 稳定。找不到退回第一页；空串 = 首次运行尚未记录。
    std::string activeTabId;
};

// --- 激活配置

enum class HotkeyModifier : std::uint8_t {
    Alt = 0,
    Control = 1,
    Shift = 2,
    Win = 3,
};

// 唤醒方式。手势需要低级键盘钩子（RegisterHotKey 表达不了），只在
// 选中时安装；默认的修饰键+主键仍是零开销注册热键。
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

// 设置界面提供的四种预设。用预设而非自由录制：录制要处理修饰键、
// 反馈与冲突检测，收益低，这四个已覆盖绝大多数诉求。
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

// 「跟随启动」的一条：KTile 启动时顺带拉起的程序。不直接存 LaunchItem：
// 那条结构带磁贴尺寸、颜色、标签归属等一整套字段，这里只关心怎么启动；
// 分开存也让清空磁贴、调整网格不波及此清单。
// 两种来源靠 itemId 是否为空区分：手动添加（空）来自设置窗口「添加…」，
// 只填一个路径，按 target 原样启动；从磁贴添加（非空）启动前按 id 取
// 磁贴当前值，参数、工作目录、管理员设置全部跟着磁贴走。下面的快照
// 字段是磁贴被删之后的兜底：删磁贴表达「不想在面板上看到它」，不等于
// 「不要开机启动」，条目保留、靠快照继续能启动。快照字段两种来源都读，
// 只在有内容时写入，手改配置给手动条目补参数同样生效。
struct FollowLaunchEntry {
    // 显示名（空时界面按 target 推测）
    std::string name;

    // 目标程序路径。允许写相对 exe 目录的相对路径（"tools\app.exe"），
    // 解析在 platform/win32/program_paths 里做。
    std::string target;

    // ---- 来源磁贴（为空 = 用户手动添加）----

    // 磁贴的稳定 id。非空时启动前按它找磁贴，以磁贴为准。
    std::string itemId;

    // ---- 添加时的快照（仅磁贴来源使用，磁贴消失后靠它启动）----
    std::vector<std::string> arguments;
    std::optional<std::string> workingDirectory;
    bool runAsAdministrator = false;

    bool fromTile() const noexcept { return !itemId.empty(); }
};

// 跟随启动列表的上限：不是省内存，是防配置写坏后每次启动拉起几百个进程。
inline constexpr std::size_t kMaximumFollowLaunchItems = 32;

// 置顶快捷键的默认值：裸 F7（无修饰）。
inline HotkeyConfig defaultTopmostHotkey() {
    HotkeyConfig config;
    config.modifiers.clear();
    config.key = "F7";
    return config;
}

struct ActivationConfig {
    HotkeyConfig hotkey;

    // 窗口置顶快捷键（切换前台窗口 WS_EX_TOPMOST）。任意修饰键 + 主键，
    // enabled=false 表示关闭该功能。与唤醒热键各自独立注册。
    HotkeyConfig topmostHotkey = defaultTopmostHotkey();

    bool startWithWindows = false;

    // 跟随启动：KTile 启动时一并启动的外部程序，已在运行的不重复启动。
    std::vector<FollowLaunchEntry> followLaunch;

    // 是否启用屏幕边缘唤起（v2 功能，先留字段）
    bool screenEdgeEnabled = false;
};

// --- 应用总配置（config.json）

struct ApplicationConfig {
    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    AppearanceConfig appearance;
    ActivationConfig activation;

    // 界面语言代码（"zh-Hans" / "en" / 后续新增），空串或 "auto" = 跟随系统。
    // 默认 auto：首次运行按系统语言选，用户显式选过后记住其选择。
    std::string language = "auto";

    bool loggingEnabled = true;
};

// --- 校验常量

// 名称/ID 长度上限
inline constexpr std::size_t kMaximumNameLength = 256;
inline constexpr std::size_t kMaximumTargetLength = 4096;
inline constexpr std::size_t kMaximumIdLength = 64;

}  // namespace core
}  // namespace ktile
