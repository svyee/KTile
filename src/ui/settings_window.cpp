#include "ui/settings_window.h"

#include "base/text_convert.h"
#include "core/data_model.h"
#include "infrastructure/i18n.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/unique_handles.h"
#include "ui/color_picker.h"
#include "ui/dialog_style.h"
#include "ui/radial_editor_dialog.h"
#include "ui/theme.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// ---------------------------------------------------------------- 取词
//
// T() 只是个短名字：真正干活的是 infrastructure::i18n::trw（按当前语言查表，缺词回退英语）。
// 返回的指针指向语言字典里的稳定存储，下次切语言之前一直有效 ——
// 所以可以直接塞进 Win32 的 LPARAM / SetWindowTextW。
const wchar_t* T(const char* key) {
    return infrastructure::i18n::trw(key).c_str();
}

// ---------------------------------------------------------------- 布局常量
//
// 全部按 96 DPI 的「逻辑像素」书写，运行时统一用 scaleFor() 缩放。
// 这样做的好处：换一台 150% 缩放屏时不必重算任何数字，只需保证
// 这里写的是设计稿尺寸。

// 窗口尺寸：左侧一列竖向标签 + 右侧一列表单。
// 「常规设置」拆成「外观 / 行为」两页后，单页只有一个方向变长，
// 所以窗口从 820×400 收窄成 700×470 —— 横向不再挤两栏，
// 150% 缩放下也不会顶到工作区顶部。
constexpr int kDialogWidth = 700;
constexpr int kDialogHeight = 470;

constexpr int kMargin = 16;          // 窗口四周边距
constexpr int kContentTop = 14;      // 内容区起始 y

// ---- 左侧竖向标签栏 ----
constexpr int kSidebarLeft = 16;
constexpr int kSidebarTop = 16;
constexpr int kSidebarWidth = 110;
constexpr int kSidebarTabHeight = 34;
constexpr int kSidebarTabGap = 4;

// ---- 右侧内容区：单列表单 ----
constexpr int kContentLeft = kSidebarLeft + kSidebarWidth + 16;        // 142
constexpr int kContentRight = kDialogWidth - kMargin;                  // 684
constexpr int kContentWidth = kContentRight - kContentLeft;            // 542
constexpr int kColumnWidth = 500;    // 表单内容宽度（卡片另加左右内边距）
// 单列布局下所有控件都以 kContentLeft 为基准。保留这个别名，
// 各分组的创建代码不必逐行改 x 坐标。
constexpr int kColumnRightX = kContentLeft;

constexpr int kRowGap = 12;          // 行间垂直间距
constexpr int kFormLabelMinWidth = 76;  // 标签列最小宽度（按语言实测，可能更宽）
constexpr int kComboWidth = 260;     // 下拉框宽度（单列下不铺满，视觉更稳）
// 内容卡片：把控件整体包进白色圆角卡片，是「现代设置页」最明显的
// 视觉特征。卡片几何只影响背景绘制，不改变任何控件的绝对坐标。
// 每页高度按各自内容量算，切页时白卡高度随之变化，灰底留白即
// 「这页到这儿为止」的视觉信号。
constexpr int kCardInset = 10;       // 卡片相对内容边界的左右外扩
constexpr int kCardTop = 8;
constexpr int kCardHeightAppearance = 410;  // 背景+磁贴+图标与标签+布局
// 语言 + 窗口 + 启动，其中「启动」含跟随启动列表（88px 的列表 + 标签行）。
constexpr int kCardHeightBehavior = 400;
constexpr int kCardHeightPlugins = 366;     // 说明+列表+按钮+脚注
// 轮盘插件页：说明 + 列表 + 按钮 + 脚注（结构与磁贴插件页一致）。
// 比磁贴插件页矮一些：它的脚注只有三行，按四行给高会在白卡下沿留一条
// 明显的空带（实测截图能看出来）。
constexpr int kCardHeightRadialPlugins = 350;

// 轮盘页：说明文字搬进 tooltip 之后只剩「三行设置 + 扇区入口」。
// 高度要跟着缩 —— 留着空白的白卡会让人以为下面还有没加载出来的内容。
constexpr int kCardHeightRadial = 262;
constexpr int kRadialPluginHintHeight = 66;
constexpr int kSliderWidth = 160;    // 滑块宽度
constexpr int kSliderValueOffset = kSliderWidth + 6;  // 百分比文字的 x 偏移
constexpr int kSpinnerWidth = 76;    // 数字输入框宽度
constexpr int kSpinnerHeight = 24;
constexpr int kButtonWidth = 78;
constexpr int kButtonHeight = 26;

// 网格范围与数据模型保持一致，改这里不会与布局引擎脱节
constexpr int kMinGrid = static_cast<int>(core::kMinimumGridColumns);
constexpr int kMaxGrid = static_cast<int>(core::kMaximumGridColumns);

// 窗口位置下拉的「展示顺序」。
//
// 与 core::WindowPlacement 的枚举值**刻意不一一对应**：枚举值要保证
// 已存配置不错位（Center 只能追加在末尾 = 6），但下拉里「屏幕中央」
// 放在第二位才顺手。所以这里用一张映射表，索引只对下拉负责。
// 文案不写在这里，只写 key：语言文件是唯一的文案来源，
// 表本身留 constexpr（key 是编译期常量），取词在创建控件时做。
struct PlacementOption {
    const char* key;
    core::WindowPlacement value;
};

constexpr PlacementOption kPlacementOptions[] = {
    {"settings.placement.mouse", core::WindowPlacement::Mouse},
    {"settings.placement.center", core::WindowPlacement::Center},
    {"settings.placement.leftBottom", core::WindowPlacement::LeftBottom},
    {"settings.placement.rightBottom", core::WindowPlacement::RightBottom},
    {"settings.placement.leftTop", core::WindowPlacement::LeftTop},
    {"settings.placement.rightTop", core::WindowPlacement::RightTop},
    {"settings.placement.last", core::WindowPlacement::Last},
};

constexpr int kPlacementOptionCount =
    static_cast<int>(sizeof(kPlacementOptions) / sizeof(kPlacementOptions[0]));

// 轮盘触发键 / 修饰键的下拉映射。
// 与 data_model 的字符串键一一对应，但顺序按「常用程度」排：
// 右键在最前（保住肌肉记忆），中键次之，侧键放最后。
struct RadialButtonOption {
    const char* key;
    core::RadialTriggerButton value;
};

constexpr RadialButtonOption kRadialButtonOptions[] = {
    {"radial.button.right", core::RadialTriggerButton::Right},
    {"radial.button.middle", core::RadialTriggerButton::Middle},
    {"radial.button.side1", core::RadialTriggerButton::Side1},
    {"radial.button.side2", core::RadialTriggerButton::Side2},
};

constexpr int kRadialButtonOptionCount = static_cast<int>(
    sizeof(kRadialButtonOptions) / sizeof(kRadialButtonOptions[0]));

// 抑制键：按着这些键的时候，轮盘一律不弹（鼠标事件原样放行）。
// 用 HotkeyModifier 而不是 RadialTriggerModifier，因为要覆盖 Win，
// 而触发修饰键里没有 Win（Win+右键 是系统手势的地盘，不做触发键）。
//
// 这里只描述「下拉里有哪些行」。选中状态不存在这张表里 —— 它存在组合框的
// item data 上（见 suppressChecked），这样保存时统一从控件读，和这一页
// 其它控件（BM_GETCHECK / CB_GETCURSEL / TBM_GETPOS）保持同一种取数方式。
struct RadialSuppressOption {
    const wchar_t* label;
    core::HotkeyModifier value;
};

constexpr RadialSuppressOption kRadialSuppressOptions[] = {
    {L"Ctrl", core::HotkeyModifier::Control},
    {L"Shift", core::HotkeyModifier::Shift},
    {L"Alt", core::HotkeyModifier::Alt},
    {L"Win", core::HotkeyModifier::Win},
};

constexpr int kRadialSuppressOptionCount = static_cast<int>(
    sizeof(kRadialSuppressOptions) / sizeof(kRadialSuppressOptions[0]));

struct RadialModifierOption {
    const char* key;
    core::RadialTriggerModifier value;
};

constexpr RadialModifierOption kRadialModifierOptions[] = {
    // 只写「无」：括号里的「独占该键」四个字把下拉撑到 110 宽，
    // 而这一行还要塞抑制键。解释挪到下拉的 tooltip 上。
    {"common.none", core::RadialTriggerModifier::None},
    {"key.ctrl", core::RadialTriggerModifier::Ctrl},
    {"key.shift", core::RadialTriggerModifier::Shift},
    {"key.alt", core::RadialTriggerModifier::Alt},
};

constexpr int kRadialModifierOptionCount = static_cast<int>(
    sizeof(kRadialModifierOptions) / sizeof(kRadialModifierOptions[0]));

// 触发方式第一行的三段横排：触发键 / 修饰键 / 抑制键。
//
// 三者是一条因果链（按住什么键 + 组合什么修饰键 + 什么情况下让路），拆成两三行
// 就看不出它们在描述同一件事。
//
// 三个下拉的宽度全部按当前语言的选项实测：中文「右键拖动」52px 用不完 84，
// 英文 "Middle-drag" 连箭头要 100 出头，写死就会把字裁掉；抑制键更是**不能**
// 再吃掉行尾的剩余宽度 —— 那会让整行撑满，看着像把窗口横向拉开了。
constexpr int kRadialLabelWidthTight = 44;   // 标签列的最小宽度
constexpr int kRadialComboMinWidth = 84;     // 下拉框最小宽度
constexpr int kComboExtraWidth = 30;         // 下拉箭头 + 左右内边距

// ---------------------------------------------------------------- 控件 ID

constexpr int kColumnsEditId = 1101;
constexpr int kRowsEditId = 1102;
constexpr int kColumnsSpinId = 1103;
constexpr int kRowsSpinId = 1104;
constexpr int kTitleAlwaysRadioId = 1123;   // 文字标签：始终显示
constexpr int kTitleDynamicRadioId = 1124;  // 文字标签：移动时显示
constexpr int kTitleNeverRadioId = 1125;    // 文字标签：始终不显示
constexpr int kTabTopRadioId = 1121;        // 标签栏位置：顶部
constexpr int kTabBottomRadioId = 1122;     // 标签栏位置：底部
constexpr int kThemeComboId = 1114;
constexpr int kBackgroundBrowseId = 1116;
constexpr int kBackgroundClearId = 1118;
constexpr int kWindowPlacementComboId = 1140;
constexpr int kWindowOpacitySliderId = 1150;    // 整体不透明度滑块
constexpr int kWindowOpacityValueId = 1151;     // 整体不透明度数值
constexpr int kTileOpacitySliderId = 1152;      // 磁贴背景不透明度滑块
constexpr int kDefaultTileColorSwatchId = 1156; // 默认底色样本（SS_OWNERDRAW）
constexpr int kDefaultTileColorClearId = 1158;  // 恢复跟随皮肤
constexpr int kIconScaleSliderId = 1154;        // 图标统一缩放滑块
constexpr int kIconScaleValueId = 1155;         // 图标统一缩放数值
constexpr int kTileOpacityValueId = 1153;       // 磁贴背景不透明度数值
constexpr int kHotkeyAltSpaceRadioId = 1141;    // 唤醒快捷键：Alt+Space
constexpr int kHotkeyCtrlSpaceRadioId = 1142;   // 唤醒快捷键：Ctrl+Space
constexpr int kHotkeyDoubleCtrlRadioId = 1143;  // 唤醒快捷键：双击 Ctrl
constexpr int kHotkeyWinKeyRadioId = 1144;      // 唤醒快捷键：单按 Win
constexpr int kStartupCheckId = 1111;
constexpr int kHintId = 1120;
constexpr int kOkId = 1130;
constexpr int kCancelId = 1131;

// ---- 分页与插件管理页 ----
//
// 分页用自绘 owner-draw 按钮竖排在左侧，而不是 SysTabControl32：
// 后者会画一块带边框的「页体」，而本对话框的各页是绝对坐标平铺的，
// 把控件塞进页体还得处理坐标重映射；按钮分页只做显隐，零几何改动。
// 「常规设置」按内容分成两页：外观（长什么样）与行为（怎么唤出）。
constexpr int kAppearanceTabId = 1135;
constexpr int kPluginsTabId = 1136;
constexpr int kBehaviorTabId = 1137;

constexpr int kPluginListId = 1138;
constexpr int kPluginInstallId = 1170;
constexpr int kPluginAddTileId = 1171;
constexpr int kPluginOpenFolderId = 1172;
constexpr int kPluginRemoveId = 1173;
constexpr int kPluginHintId = 1174;

// ---- 轮盘菜单页 ----
//
// 这一页只管「什么时候呼出」：启用开关、触发键 + 修饰键、拖动阈值、
// 外甩取消半径。扇区动作与按应用匹配在独立对话框里编辑（radial_editor），
// 那一块是列表 + 表单的复合结构，塞不进这里的单列表单。
constexpr int kRadialTabId = 1180;
constexpr int kRadialEnabledCheckId = 1181;
constexpr int kRadialButtonComboId = 1182;
constexpr int kRadialModifierComboId = 1183;
constexpr int kRadialThresholdSliderId = 1184;
constexpr int kRadialThresholdValueId = 1185;
constexpr int kRadialCancelSliderId = 1186;
constexpr int kRadialCancelValueId = 1187;
constexpr int kRadialEditButtonId = 1188;
constexpr int kRadialSummaryId = 1189;
// 抑制键：一个「可勾选的下拉」。Win32 没有原生的多选下拉，实做法是
// CBS_DROPDOWNLIST + CBS_OWNERDRAWFIXED：字段（itemID == -1）自己画汇总文字，
// 列表行自己画 ☑/☐，再把列表的子类化拦住单击 —— 否则点一下就收起，
// 勾第二个还得再展开一次。
constexpr int kRadialSuppressComboId = 1191;
// 1190 / 1195 是「说明行」拆掉后空出来的两个位，现在给抑制键的标签用：
// 选修饰键时整组连标签一起变灰，标签也得有个句柄。
constexpr int kRadialSuppressLabelId = 1190;

// ---- 轮盘插件页 ----
//
// ID 只能落在 1196~1200 这五个空位里：1201 起是分组标题的连续区间
// （kSectionHeaderIdBase），插进去会把「灰色标题」判定连根拔起。
constexpr int kRadialPluginsTabId = 1196;
constexpr int kRadialPluginListId = 1197;
constexpr int kRadialPluginOpenJsonId = 1198;
constexpr int kRadialPluginOpenFolderId = 1199;
constexpr int kRadialPluginRescanId = 1200;

// 滑块范围：阈值太小会被手抖误触，太大则拖半天不出轮盘；
// 取消半径给 150~400 足够覆盖「刚好越过花瓣」到「大臂横甩」。
// 下限 150 是因为花瓣外缘在 142 DIP，比它更小的值会被窗口夹回 142，
// 滑杆留在 100 只会给用户「拖了没反应」的错觉。
constexpr int kRadialThresholdMin = 4;
constexpr int kRadialThresholdMax = 40;
constexpr int kRadialCancelMin = 150;
constexpr int kRadialCancelMax = 400;

// 插件页：说明 + 列表 + 操作按钮 + 脚注（均在右侧内容区内自上而下排布）
constexpr int kPluginIntroTop = kContentTop;
constexpr int kPluginIntroHeight = 18;
constexpr int kPluginListTop = kPluginIntroTop + kPluginIntroHeight + 6;
constexpr int kPluginListHeight = 196;
constexpr int kPluginButtonTop = kPluginListTop + kPluginListHeight + 8;
constexpr int kPluginHintTop = kPluginButtonTop + kButtonHeight + 8;
// 内容区收窄到 542 后，脚注第三行会折成两行，高度要给够，
// 否则 STATIC 的白底会从卡片下沿冒出去。
constexpr int kPluginHintHeight = 88;
constexpr int kPluginButtonWidth = 96;

// 分组标题的控件 ID。用一段连续区间而不是逐个常量，是为了在
// WM_CTLCOLORSTATIC 里一次判定「这批是分组标题，画成灰色」。
constexpr int kSectionHeaderIdBase = 1201;
constexpr int kSectionAppearanceId = 1201;  // 背景
constexpr int kSectionLayoutId = 1202;
constexpr int kSectionWindowId = 1203;
constexpr int kSectionStartupId = 1204;
constexpr int kSectionTileId = 1205;        // 磁贴
constexpr int kSectionIconLabelId = 1206;   // 图标与标签
constexpr int kSectionRadialTriggerId = 1207;  // 轮盘：触发方式
constexpr int kSectionRadialSectorId = 1208;   // 轮盘：扇区动作
constexpr int kSectionLanguageId = 1209;       // 语言
constexpr int kSectionHeaderIdEnd = 1210;
// 语言下拉。1211 起在分组标题区间之外，不会被 WM_CTLCOLORSTATIC 当成
// 「灰色小标题」处理。
constexpr int kLanguageComboId = 1211;

// ---- 跟随启动 ----
//
// 一个「图标 + 名称 + 路径」的小列表加两个按钮。放列表而不是一行一个
// 复选框：条目里要显示图标（用户一眼认出是哪个程序），复选框做不出这个。
constexpr int kFollowListId = 1212;
constexpr int kFollowAddId = 1213;
constexpr int kFollowRemoveId = 1214;
// 列表图标尺寸与列表高度（逻辑像素）
constexpr int kFollowIconSize = 18;
constexpr int kFollowListHeight = 88;

// 控件 ID 到「窗口属性名」的映射：把控件句柄挂在对话框上，
// 避免到处 GetDlgItem（虽然也能用，但属性查找更直白）。
constexpr const wchar_t* kPropColumnsEdit = L"KTileSetColumnsEdit";
constexpr const wchar_t* kPropRowsEdit = L"KTileSetRowsEdit";
constexpr const wchar_t* kPropHint = L"KTileSetHint";
constexpr const wchar_t* kPropFont = L"KTileSetFont";
// 分页标签「选中态」用的加粗字体（由 kPropFont 派生，随窗口销毁）。
constexpr const wchar_t* kPropTabFont = L"KTileSetTabFont";
constexpr const wchar_t* kPropSectionFont = L"KTileSetSectionFont";
constexpr const wchar_t* kPropPluginList = L"KTileSetPluginList";
constexpr const wchar_t* kPropRadialPluginList = L"KTileSetRadialPluginList";
// 悬停提示窗口（整个设置窗口共用一个，控件只是 TTM_ADDTOOL 挂上去）
constexpr const wchar_t* kPropTooltip = L"KTileSetTooltip";

// ---------------------------------------------------------------- 状态

struct SettingsDialogState {
    SettingsResult result;

    // 初值：用于判断是否真的改动过
    std::uint16_t initialColumns = 0;
    std::uint16_t initialRows = 0;
    bool initialShowLabels = true;
    bool initialAlwaysShowTitle = true;
    bool initialTabBarAtBottom = false;
    core::WindowPlacement initialWindowPlacement = core::WindowPlacement::Mouse;
    core::HotkeyPreset initialHotkeyPreset = core::HotkeyPreset::AltSpace;
    bool initialStartup = false;

    // 不透明度：显示的是「当前实际生效的百分比」。
    // 磁贴背景的「跟随皮肤」在界面上表现为皮肤自带的那个值，用户一旦
    // 拖过滑块就变成显式值 —— 所以要用 touched 标记区分「拖动过」与
    // 「原样没动」，否则点确定会把「跟随皮肤」写死成一个数字。
    int initialWindowOpacity = 100;
    int initialTileOpacity = 24;
    bool windowOpacityTouched = false;
    bool tileOpacityTouched = false;
    int initialIconScale = 90;
    bool iconScaleTouched = false;

    // 全局默认磁贴底色：初始值 / 当前选择 / 是否动过。
    // 用「是否动过」而不是直接比值，才能把「取消选色」这种回到
    // 空值的变化也识别出来。
    std::optional<std::uint32_t> initialDefaultTileColor;
    std::optional<std::uint32_t> chosenDefaultTileColor;
    bool defaultTileColorTouched = false;

    // 界面语言（配置里的原值："auto" 或具体语言代码）
    std::string initialLanguage;

    // 跟随启动：初值（判断是否改动过）与草稿（界面上的当前内容）。
    // 「添加 / 移除」只改草稿，点确定才随 SettingsResult 交回应用层。
    std::vector<core::FollowLaunchEntry> initialFollowLaunch;
    std::vector<core::FollowLaunchEntry> followLaunch;
    // 列表图标表（LVSIL_SMALL）。随对话框销毁。
    HIMAGELIST followIcons = nullptr;

    // 皮肤与背景图
    std::wstring initialThemeName;      // 当前皮肤显示名（空 = 默认）
    std::wstring pickedBackgroundPath;  // 本次会话新选的图片（绝对路径）
    bool backgroundCleared = false;     // 用户点了「清除」

    // 当前布局已占用的行列数（用于提示文案）
    std::uint16_t columnsInUse = 0;
    std::uint16_t rowsInUse = 0;

    // 实时预览回调（不拥有；指向 showSettingsWindow 栈上的对象）
    const SettingsPreviewHooks* preview = nullptr;

    // 插件管理页回调（不拥有；为空时该页只显示说明文字）
    const SettingsPluginHooks* plugins = nullptr;

    // 轮盘插件页回调（不拥有；为空时该页只显示说明文字）
    const SettingsRadialPluginHooks* radialPlugins = nullptr;

    // 轮盘菜单配置的草稿。
    // 子对话框（radial_editor）直接改这份副本，点设置窗口的「确定」才
    // 随 SettingsResult 交回应用层 —— 「取消」就整份丢弃，无需回滚。
    core::RadialConfig radial;

    // 分页：0 = 外观，1 = 行为，2 = 插件管理，3 = 轮盘，4 = 轮盘插件。
    // 各页控件在 WM_CREATE 里已经按绝对坐标摆好，切页只做 ShowWindow，
    // 不重新布局 —— 这样切回来时滚动条/焦点都还是原来的样子。
    int activePage = 0;
    std::vector<HWND> appearanceControls;
    std::vector<HWND> behaviorControls;
    std::vector<HWND> pluginControls;
    std::vector<HWND> radialControls;
    std::vector<HWND> radialPluginControls;
    // 列表行与 ListView 下标一一对应，选中项据此反查 id
    std::vector<SettingsPluginInfo> pluginRows;
    std::vector<SettingsRadialPluginInfo> radialPluginRows;

    bool accepted = false;
};

// 读下拉框当前选中项的文字。没有选中项返回空串。
std::wstring comboSelectedText(HWND dialog, const int controlId) {
    HWND combo = GetDlgItem(dialog, controlId);
    if (combo == nullptr) {
        return std::wstring();
    }
    const int selection =
        static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selection < 0) {
        return std::wstring();
    }
    const int length = static_cast<int>(
        SendMessageW(combo, CB_GETLBTEXTLEN, selection, 0));
    if (length <= 0 || length >= 256) {
        return std::wstring();
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, selection,
                 reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

constexpr const wchar_t* kSettingsDialogClass = L"KTile.SettingsDialog";

// 对话框关闭通知（仅用于结束本对话框的嵌套模态循环）。
//
// 用 WM_APP 段的自定义消息而不是 WM_QUIT：WM_QUIT 是线程级的、
// 且粘性 —— 会连带把主消息循环也终止掉（见 WM_DESTROY 处的说明）。
constexpr UINT kDialogCloseMessage = WM_APP + 0x90U;

// 按 DPI 缩放像素值（与 tile_context_menu.cpp 保持同一策略）
int scaleFor(HWND window, const int value) {
    HDC dc = GetDC(window);
    if (dc == nullptr) {
        return value;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return MulDiv(value, dpi > 0 ? dpi : 96, 96);
}

HFONT dialogFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0) != FALSE) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// 定义在窗口过程之后（它要用到本文件的其余布局常量），这里先声明。
void positionSettingsWindow(HWND dialog, HWND owner,
                            bool coverCenterFallback);

// 从编辑框读一个整数并夹到 [kMinGrid, kMaxGrid]。
// 读不到（空串/非数字）时返回 fallback。
int readSpinnerValue(HWND dialog, const wchar_t* propName, const int fallback) {
    HWND edit = static_cast<HWND>(GetPropW(dialog, propName));
    if (edit == nullptr) {
        return fallback;
    }
    wchar_t buffer[16] = {};
    GetWindowTextW(edit, buffer, 16);

    // 用 wcstol 而不是 _wtoi：能区分「真的是 0」与「压根不是数字」，
    // 虽然这里都归到 fallback，但语义更清楚。
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    if (end == buffer) {
        return fallback;
    }
    if (value < kMinGrid) {
        return kMinGrid;
    }
    if (value > kMaxGrid) {
        return kMaxGrid;
    }
    return static_cast<int>(value);
}

// 把编辑框内容规范化写回（用户可能输入 "007" 或越界值）
void writeSpinnerValue(HWND dialog, const wchar_t* propName, const int value) {
    HWND edit = static_cast<HWND>(GetPropW(dialog, propName));
    if (edit == nullptr) {
        return;
    }
    const std::wstring text = std::to_wstring(value);
    SetWindowTextW(edit, text.c_str());
}

// 更新提示文案。
//
// 只在「新尺寸比当前布局实际占用更小」时警告 —— 否则不打扰用户。
// 缩小网格不会删磁贴，超出的磁贴会被布局引擎重新安置到可用区域，
// 所以这里说的是「会重排」而不是「会丢失」。
void refreshHint(HWND dialog, const SettingsDialogState* state) {
    HWND hint = static_cast<HWND>(GetPropW(dialog, kPropHint));
    if (hint == nullptr || state == nullptr) {
        return;
    }

    const int columns = readSpinnerValue(dialog, kPropColumnsEdit,
                                         state->initialColumns);
    const int rows = readSpinnerValue(dialog, kPropRowsEdit,
                                      state->initialRows);

    std::wstring text;
    if (columns < static_cast<int>(state->columnsInUse) ||
        rows < static_cast<int>(state->rowsInUse)) {
        text = T("settings.gridHintRearrange");
    } else {
        text = T("settings.gridHintRange");
    }
    SetWindowTextW(hint, text.c_str());
}

// ---------------------------------------------------------------- 语言下拉

// 第 0 项固定是「跟随系统」，之后按 lang 目录里的语言代码排序。
//
// 「跟随系统」这一项要把实际落到的语言写在后面（"跟随系统 · 简体中文"）：
// 只写「跟随系统」的话，用户没法知道现在到底跟着哪一门 —— 尤其是系统语言
// 是英文、界面却是中文的时候，会怀疑设置没生效。
void fillLanguageCombo(HWND combo, const std::string& current) {
    if (combo == nullptr) {
        return;
    }
    const std::string systemLanguage =
        infrastructure::i18n::systemLanguageCode();
    std::wstring autoText =
        infrastructure::i18n::trw("settings.language.auto");
    for (const auto& language : infrastructure::i18n::languages()) {
        if (language.code == systemLanguage) {
            autoText += L" · " + base::utf8ToWide(language.name);
            break;
        }
    }
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(autoText.c_str()));

    int selection = 0;
    int index = 1;
    for (const auto& language : infrastructure::i18n::languages()) {
        const std::wstring name = base::utf8ToWide(language.name);
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
        if (language.code == current) {
            selection = index;
        }
        ++index;
    }
    // 配置里写了个已经不存在（语言文件被删）的代码：停在「跟随系统」上，
    // 比停在一个空项上更接近用户的意思。
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
}

// 读回用户选的语言代码；第 0 项 = "auto"。
std::string selectedLanguageCode(HWND dialog) {
    HWND combo = GetDlgItem(dialog, kLanguageComboId);
    if (combo == nullptr) {
        return std::string(infrastructure::i18n::kAutoLanguage);
    }
    const int selection = static_cast<int>(
        SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selection <= 0) {
        return std::string(infrastructure::i18n::kAutoLanguage);
    }
    const auto& list = infrastructure::i18n::languages();
    const std::size_t index = static_cast<std::size_t>(selection - 1);
    if (index >= list.size()) {
        return std::string(infrastructure::i18n::kAutoLanguage);
    }
    return list[index].code;
}

// ---------------------------------------------------------------- 分页与插件列表

// 取列表当前选中的插件 id。没有选中项返回空串。
std::wstring selectedPluginId(HWND dialog, const SettingsDialogState* state) {
    if (state == nullptr) {
        return std::wstring();
    }
    HWND list = static_cast<HWND>(GetPropW(dialog, kPropPluginList));
    if (list == nullptr) {
        return std::wstring();
    }
    const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->pluginRows.size()) {
        return std::wstring();
    }
    return base::utf8ToWide(state->pluginRows[static_cast<std::size_t>(index)].id);
}

// 重新填充插件列表。
//
// 每次切到插件页、安装/删除成功后都会调用 —— 插件的真相在磁盘上，
// 缓存一份列表到对话框生命周期结束只会看到过期数据。
void refreshPluginList(HWND dialog, SettingsDialogState* state) {
    HWND list = static_cast<HWND>(GetPropW(dialog, kPropPluginList));
    if (list == nullptr || state == nullptr) {
        return;
    }

    // 记住选中项：刷新后尽量停在原来那个插件上，避免每次操作后跳回顶部
    std::wstring previous;
    const int previousIndex = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (previousIndex >= 0 &&
        static_cast<std::size_t>(previousIndex) < state->pluginRows.size()) {
        previous = selectedPluginId(dialog, state);
    }

    state->pluginRows.clear();
    ListView_DeleteAllItems(list);

    if (state->plugins != nullptr && state->plugins->list) {
        state->pluginRows = state->plugins->list();
    }

    for (std::size_t i = 0; i < state->pluginRows.size(); ++i) {
        const SettingsPluginInfo& row = state->pluginRows[i];
        const std::wstring name = base::utf8ToWide(row.name);
        const std::wstring id = base::utf8ToWide(row.id);
        const std::wstring source = base::utf8ToWide(row.sourceLabel);
        const std::wstring refresh =
            row.refreshSeconds > 0
                ? std::to_wstring(row.refreshSeconds) + infrastructure::i18n::trw("unit.seconds")
                : std::wstring(L"—");
        const std::wstring visibleRefresh =
            row.visibleRefreshSeconds > 0
                ? std::to_wstring(row.visibleRefreshSeconds) + infrastructure::i18n::trw("unit.seconds")
                : std::wstring(L"—");

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(name.c_str());
        const int index = ListView_InsertItem(list, &item);
        if (index < 0) {
            continue;
        }
        ListView_SetItemText(list, index, 1, const_cast<LPWSTR>(id.c_str()));
        ListView_SetItemText(list, index, 2, const_cast<LPWSTR>(source.c_str()));
        ListView_SetItemText(list, index, 3,
                             const_cast<LPWSTR>(refresh.c_str()));
        ListView_SetItemText(list, index, 4,
                             const_cast<LPWSTR>(visibleRefresh.c_str()));
    }

    // 恢复选中项
    if (!previous.empty()) {
        for (std::size_t i = 0; i < state->pluginRows.size(); ++i) {
            if (base::utf8ToWide(state->pluginRows[i].id) == previous) {
                ListView_SetItemState(list, static_cast<int>(i),
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }

    // 按钮的可用性跟着列表走：没选中任何插件时，「添加磁贴 / 打开目录 /
    // 删除」点了也没有意义，灰掉比弹提示更直接。
    const bool hasSelection = !selectedPluginId(dialog, state).empty();
    // 「打开目录」始终可用（没选中时打开插件根目录）；
    // 其余两项必须有选中项才有意义。
    const int buttonIds[] = {kPluginAddTileId, kPluginRemoveId};
    for (const int id : buttonIds) {
        HWND button = GetDlgItem(dialog, id);
        if (button != nullptr) {
            EnableWindow(button, hasSelection ? TRUE : FALSE);
        }
    }
}

// 轮盘页的概况行：通用轮盘配了几个扇区 + 黑名单有几条。
//
// 为什么需要这一行：扇区动作在子对话框里编辑，回到设置页后界面上只看得到
// 触发方式。给一句「4 / 8 个扇区已配置」，用户才能确认自己的改动还在。
void refreshRadialSummary(HWND dialog, const SettingsDialogState* state) {
    if (state == nullptr) {
        return;
    }
    HWND summary = GetDlgItem(dialog, kRadialSummaryId);
    if (summary == nullptr) {
        return;
    }

    const core::RadialMenu& menu = state->radial.menu;
    int configured = 0;
    for (const auto& action : menu.actions) {
        if (!action.isEmpty()) {
            ++configured;
        }
    }
    // 用占位符而不是自己拼句子：英文的「x of y」和中文的「x / y 个」语序不同，
    // 拼接会把语序写死在代码里。
    std::wstring text = infrastructure::i18n::formatW(
        "settings.radial.summary",
        {std::to_wstring(configured), std::to_wstring(menu.sectorCount)});
    if (!state->radial.blacklist.empty()) {
        text += infrastructure::i18n::formatW(
            "settings.radial.summaryBlacklist",
            {std::to_wstring(state->radial.blacklist.size())});
    }
    SetWindowTextW(summary, text.c_str());
}

// ---------------------------------------------------------------- 悬停提示

// 设置项旁边成段的灰字说明，看起来像「说明书」而不是「设置」。改成：控件上
// 只留名字，解释挂在鼠标悬停上。想知道的人停一下就有，不想知道的人眼前干净。
//
// 用系统 TOOLTIPS_CLASS + TTF_SUBCLASS：subclass 模式由 tooltip 自己消化
// 鼠标消息，不必给每个控件写 TrackMouseEvent，也不会因为控件被重父化而失效。
// Win7 自带（comctl32 v5），不引入任何新依赖，符合本项目的零依赖底线。
HWND ensureTooltip(HWND owner, HFONT font) {
    HWND tip = static_cast<HWND>(GetPropW(owner, kPropTooltip));
    if (tip != nullptr) {
        return tip;
    }
    tip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tip == nullptr) {
        return nullptr;
    }
    SetWindowPos(tip, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 不给上限的话，一句长解释会被拉成一整条横贯屏幕的文字
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0,
                 static_cast<LPARAM>(scaleFor(owner, 260)));
    if (font != nullptr) {
        SendMessageW(tip, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SetPropW(owner, kPropTooltip, tip);
    return tip;
}

void addTooltip(HWND owner, HWND tool, const wchar_t* text) {
    if (owner == nullptr || tool == nullptr || text == nullptr) {
        return;
    }
    HWND tip = ensureTooltip(owner,
                             static_cast<HFONT>(GetPropW(owner, kPropFont)));
    if (tip == nullptr) {
        return;
    }
    TOOLINFOW info{};
    // 必须用 V1 尺寸（56 字节），不能用 sizeof(TOOLINFOW)（72 字节）。
    //
    // 这是踩出来的：本程序没有 comctl32 v6 的 manifest，进程加载的是系统自带的
    // v5.82，它的 TOOLINFO 还是 IE4 时代那份（到 lpszText 为止）。头文件里的
    // TOOLINFOW 多出 lParam / lpReserved（v6 才有），按 sizeof 传过去，v5 认为
    // 「结构尺寸不认识」，TTM_ADDTOOL 直接返回 FALSE —— 不报错、不写事件日志，
    // 表现就是「悬停死活不弹」，只能靠 TTM_GETTOOLCOUNT 看出挂载数还是 0。
    // V1 尺寸在 v6 下同样被接受（向后兼容），所以这是两个版本都安全的写法。
    info.cbSize = TTTOOLINFOW_V1_SIZE;
    // TTF_IDISHWND：uId 是控件句柄本身，一个控件一条 tip，不用编 ID
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = owner;
    info.uId = reinterpret_cast<UINT_PTR>(tool);
    info.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

// ---------------------------------------------------------------- 抑制键下拉
//
// 用户要的是「一个下拉框」，不是四个复选框。除了少占版面，还有一个实在的
// 好处：四个复选框并排时，光看勾选框很难一眼说出「按住这几个键都不弹」，
// 收起状态下更是完全没有汇总；下拉框收起来就写着 "Ctrl、Shift"。
//
// Win32 没有原生的多选下拉，这里的做法是 CBS_DROPDOWNLIST +
// CBS_OWNERDRAWFIXED：
//   - 勾选状态放在 item data（1 = 勾上），保存时从控件读，与这一页其它控件
//     （BM_GETCHECK / CB_GETCURSEL / TBM_GETPOS）保持同一种取数方式；
//   - 字段自己画汇总文字（WM_DRAWITEM 的 itemID == -1）；
//   - 列表子类化后拦鼠标单击：点一行只切换勾选、不收起列表。
//
// 两个「能不能做到」都实测过（build/probe_combo.cpp）：
//   - CB_SETCURSEL(-1)（没有任何选中项）时，字段照样会收到 itemID == -1 的
//     WM_DRAWITEM —— 所以不需要塞一行假的「摘要项」来撑住显示；
//   - GetComboBoxInfo 在还没下拉过时就能拿到 hwndList，创建完当场就能子类化，
//     不用等第一次展开。

bool suppressChecked(HWND combo, int index) {
    if (combo == nullptr || index < 0 || index >= kRadialSuppressOptionCount) {
        return false;
    }
    return SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0) == 1;
}

void setSuppressChecked(HWND combo, int index, bool checked) {
    if (combo == nullptr || index < 0 || index >= kRadialSuppressOptionCount) {
        return;
    }
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index),
                 checked ? 1 : 0);
}

// 收起状态显示的文字：勾了几个就列几个，一个没勾写「无」。
// 用顿号而不是加号：这里表达的是「任意一个按住都算」，不是「一起按住」。
std::wstring suppressSummary(HWND combo) {
    std::wstring text;
    for (int i = 0; i < kRadialSuppressOptionCount; ++i) {
        if (!suppressChecked(combo, i)) {
            continue;
        }
        if (!text.empty()) {
            text += L"、";
        }
        text += kRadialSuppressOptions[i].label;
    }
    if (text.empty()) {
        text = T("common.none");
    }
    return text;
}

void fillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (brush != nullptr) {
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }
}

// 勾选方格用 DrawFrameControl 画，不自己拼线，也不依赖字体里有没有 ☑。
// 它同时负责禁用态的灰化，省掉一条「灰了但还看得出是勾」的分支。
void paintSuppressRow(HDC dc, const RECT& rect, HFONT font,
                      const wchar_t* label, bool checked, bool selected,
                      bool disabled) {
    fillRectColor(dc, rect, selected ? dialog_style::accent()
                                     : dialog_style::card());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled
                        ? dialog_style::secondaryText()
                        : (selected ? dialog_style::accentText()
                                    : dialog_style::text()));

    // 勾选方格按 DPI 定尺寸，并且不许顶满行高（行高由字体决定，撑满会贴边）
    const int rowHeight = rect.bottom - rect.top;
    int boxSide = MulDiv(13, GetDeviceCaps(dc, LOGPIXELSY), 96);
    if (boxSide > rowHeight - 4) {
        boxSide = rowHeight - 4;
    }
    if (boxSide < 9) {
        boxSide = 9;
    }
    RECT box{};
    box.left = rect.left + 8;
    box.right = box.left + boxSide;
    box.top = rect.top + (rowHeight - boxSide) / 2;
    box.bottom = box.top + boxSide;
    UINT flags = DFCS_BUTTONCHECK;
    if (checked) flags |= DFCS_CHECKED;
    if (disabled) flags |= DFCS_INACTIVE;
    DrawFrameControl(dc, &box, DFC_BUTTON, flags);

    HFONT previous = font != nullptr ? static_cast<HFONT>(SelectObject(dc, font))
                                     : nullptr;
    RECT text = rect;
    text.left = box.right + 8;
    text.right -= 6;
    DrawTextW(dc, label, -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous != nullptr) {
        SelectObject(dc, previous);
    }
}

void paintSuppressField(HDC dc, const RECT& rect, HFONT font,
                        const std::wstring& summary, bool disabled) {
    fillRectColor(dc, rect, dialog_style::card());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? dialog_style::secondaryText()
                              : dialog_style::text());
    HFONT previous = font != nullptr ? static_cast<HFONT>(SelectObject(dc, font))
                                     : nullptr;
    RECT text = rect;
    text.left += 8;
    text.right -= 20;   // 右端是下拉箭头，别压上去
    DrawTextW(dc, summary.c_str(), -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous != nullptr) {
        SelectObject(dc, previous);
    }
}

// 列表的子类过程：单击 = 切换勾选，且不收起列表。
//
// 不这么做的话，CBS_DROPDOWNLIST 点一行就收起并选中那一行 —— 勾第二个键要
// 再展开一次，四个键勾完是八次点击。这里把成对的按下/抬起都吞掉，列表就
// 停在那儿等用户勾完；点列表外面仍然按原生逻辑收起（那条路径走 combo 自己
// 的捕获，不经过这里）。
LRESULT CALLBACK suppressListProc(HWND list, UINT message, WPARAM wParam,
                                  LPARAM lParam, UINT_PTR /*subclassId*/,
                                  DWORD_PTR refData) {
    // 组合框句柄走 refData 传进来。**不能**用 GetParent(list) 反推：
    // 实测 ComboLBox 的父窗口既不是组合框也不是对话框（是一个内部窗口，
    // GetAncestor(list, GA_ROOT) 就是它自己），拿错句柄的结果是「点一行
    // 列表不收起、但勾选没变」—— 最难查的那种静默失败。
    HWND combo = reinterpret_cast<HWND>(refData);
    if (message == WM_LBUTTONDOWN) {
        // LB_ITEMFROMPOINT：低位是行号，高位非零表示点在行外的空白处
        const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, lParam);
        if (HIWORD(hit) == 0) {
            const int index = static_cast<int>(LOWORD(hit));
            if (combo != nullptr && index >= 0 &&
                index < kRadialSuppressOptionCount) {
                setSuppressChecked(combo, index,
                                   !suppressChecked(combo, index));
                InvalidateRect(list, nullptr, TRUE);
                // 收起后显示的汇总也要跟着变，否则要等下次展开才刷新
                InvalidateRect(combo, nullptr, TRUE);
                return 0;
            }
        }
    } else if (message == WM_LBUTTONUP) {
        return 0;
    }
    return DefSubclassProc(list, message, wParam, lParam);
}

// 给抑制键下拉挂上列表子类。条目与初始勾选都从 radial 配置来。
void initSuppressCombo(HWND combo, const core::RadialConfig& radial) {
    if (combo == nullptr) {
        return;
    }
    for (int i = 0; i < kRadialSuppressOptionCount; ++i) {
        const RadialSuppressOption& option = kRadialSuppressOptions[i];
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(option.label));
        const auto& held = radial.trigger.suppressWhileHeld;
        const bool checked =
            std::find(held.begin(), held.end(), option.value) != held.end();
        setSuppressChecked(combo, i, checked);
    }
    // 没有「当前项」这个概念：收起的字段是自绘的汇总文字，见 paintSuppressField
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);

    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(combo, &info) != FALSE && info.hwndList != nullptr) {
        SetWindowSubclass(info.hwndList, suppressListProc, 1,
                          reinterpret_cast<DWORD_PTR>(combo));
    }
}

// ---------------------------------------------------------------- 轮盘触发

// 修饰键不是「无」时，抑制键整组失效。
//
// 理由（用户提的规则，也确实是这套状态机本来的语义）：选了修饰键，那么「不按
// 修饰键」这件事本身就是不触发，再让抑制键参与判断等于让同一个键既必须按、
// 又必须不按。所以此时抑制键不是「另一个选择」，而是一条死规则。
//
// 灰显而不是清空勾选：切回「无」时用户之前的选择还在，不用重勾一遍。
void refreshRadialSuppressEnabled(HWND dialog) {
    if (dialog == nullptr) {
        return;
    }
    bool usable = true;
    HWND modifierCombo = GetDlgItem(dialog, kRadialModifierComboId);
    if (modifierCombo != nullptr) {
        const int selection = static_cast<int>(
            SendMessageW(modifierCombo, CB_GETCURSEL, 0, 0));
        if (selection >= 0 && selection < kRadialModifierOptionCount) {
            usable = core::suppressionApplies(
                kRadialModifierOptions[selection].value);
        }
    }
    const BOOL enable = usable ? TRUE : FALSE;
    EnableWindow(GetDlgItem(dialog, kRadialSuppressLabelId), enable);
    EnableWindow(GetDlgItem(dialog, kRadialSuppressComboId), enable);
    // 灰掉时自绘的字段不会自己重画（原生控件靠 WM_ENABLE 走系统绘制，我们不
    // 走那条路），补一次重绘让文字真的变灰 —— 否则「灰了」只体现在点不动上。
    HWND combo = GetDlgItem(dialog, kRadialSuppressComboId);
    if (combo != nullptr) {
        InvalidateRect(combo, nullptr, TRUE);
    }
}

// ---------------------------------------------------------------- 轮盘插件列表

// 取列表当前选中的轮盘插件 id。没有选中项返回空串。
std::wstring selectedRadialPluginId(HWND dialog,
                                    const SettingsDialogState* state) {
    if (state == nullptr) {
        return std::wstring();
    }
    HWND list = static_cast<HWND>(GetPropW(dialog, kPropRadialPluginList));
    if (list == nullptr) {
        return std::wstring();
    }
    const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (index < 0 ||
        static_cast<std::size_t>(index) >= state->radialPluginRows.size()) {
        return std::wstring();
    }
    return base::utf8ToWide(
        state->radialPluginRows[static_cast<std::size_t>(index)].id);
}

// 重新填充轮盘插件列表。
//
// 与磁贴插件页同一策略：真相在磁盘上，每次切页 / 重新扫描后都重新问应用层要，
// 不在对话框里缓存「刚才扫描的结果」。
void refreshRadialPluginList(HWND dialog, SettingsDialogState* state) {
    HWND list = static_cast<HWND>(GetPropW(dialog, kPropRadialPluginList));
    if (list == nullptr || state == nullptr) {
        return;
    }

    std::wstring previous;
    const int previousIndex = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (previousIndex >= 0 &&
        static_cast<std::size_t>(previousIndex) < state->radialPluginRows.size()) {
        previous = selectedRadialPluginId(dialog, state);
    }

    state->radialPluginRows.clear();
    ListView_DeleteAllItems(list);
    if (state->radialPlugins != nullptr && state->radialPlugins->list) {
        state->radialPluginRows = state->radialPlugins->list();
    }

    for (std::size_t i = 0; i < state->radialPluginRows.size(); ++i) {
        const SettingsRadialPluginInfo& row = state->radialPluginRows[i];
        const std::wstring status = row.valid ? T("settings.radialPlugins.loaded") : T("settings.radialPlugins.skipped");
        const std::wstring name = base::utf8ToWide(row.name.empty() ? row.id
                                                                   : row.name);
        const std::wstring processes = base::utf8ToWide(row.processes);
        // 只写「动作数 / 扇区数」：列头已经是「扇区」，再写一次「扇区」是
        // 「8 / 8 扇区」这种读起来别扭的重复。两个数不相等基本就是配错了。
        const std::wstring sectors = std::to_wstring(row.actionCount) + L" / " +
                                     std::to_wstring(row.sectorCount);
        const std::wstring note = row.valid ? std::wstring(L"")
                                            : base::utf8ToWide(row.error);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(status.c_str());
        const int index = ListView_InsertItem(list, &item);
        if (index < 0) {
            continue;
        }
        ListView_SetItemText(list, index, 1, const_cast<LPWSTR>(name.c_str()));
        ListView_SetItemText(list, index, 2,
                             const_cast<LPWSTR>(processes.c_str()));
        ListView_SetItemText(list, index, 3,
                             const_cast<LPWSTR>(sectors.c_str()));
        ListView_SetItemText(list, index, 4, const_cast<LPWSTR>(note.c_str()));
    }

    if (!previous.empty()) {
        for (std::size_t i = 0; i < state->radialPluginRows.size(); ++i) {
            if (base::utf8ToWide(state->radialPluginRows[i].id) == previous) {
                ListView_SetItemState(list, static_cast<int>(i),
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }

    // 没有选中项时「打开 JSON / 打开目录」都无从下手（不知道打开哪个），
    // 灰掉比点了弹提示更直接。「重新扫描」永远可用。
    const bool hasSelection = !selectedRadialPluginId(dialog, state).empty();
    const int selectionIds[] = {kRadialPluginOpenJsonId,
                                kRadialPluginOpenFolderId};
    for (const int id : selectionIds) {
        HWND button = GetDlgItem(dialog, id);
        if (button != nullptr) {
            EnableWindow(button, hasSelection ? TRUE : FALSE);
        }
    }
}

// 切换分页：每页的控件各管各的，互不重叠。
void switchSettingsPage(HWND dialog, SettingsDialogState* state,
                        const int page) {    if (state == nullptr) {
        return;
    }
    state->activePage = page;

    for (const HWND control : state->appearanceControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (const HWND control : state->behaviorControls) {
        ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
    }
    for (const HWND control : state->pluginControls) {
        ShowWindow(control, page == 2 ? SW_SHOW : SW_HIDE);
    }
    for (const HWND control : state->radialControls) {
        ShowWindow(control, page == 3 ? SW_SHOW : SW_HIDE);
    }
    for (const HWND control : state->radialPluginControls) {
        ShowWindow(control, page == 4 ? SW_SHOW : SW_HIDE);
    }

    // 自绘标签不靠勾选态，切页后直接让四枚按钮重画；
    // 卡片高度也按页变化，所以整窗也要重画一次。
    InvalidateRect(GetDlgItem(dialog, kAppearanceTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kBehaviorTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kPluginsTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kRadialTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kRadialPluginsTabId), nullptr, TRUE);
    InvalidateRect(dialog, nullptr, TRUE);

    if (page == 2) {
        refreshPluginList(dialog, state);
    } else if (page == 3) {
        refreshRadialSummary(dialog, state);
    } else if (page == 4) {
        refreshRadialPluginList(dialog, state);
    }
}

// 画一枚竖向分页标签：选中态是「浅强调色圆角胶囊 + 强调色加粗文字」，
// 未选中为灰字。比系统按钮的左边竖条更接近现代设置页的导航样式。
void paintSettingsTab(const HDC dc, const RECT& rect, const wchar_t* text,
                      const bool active, HFONT normalFont, HFONT activeFont) {
    if (dc == nullptr || text == nullptr) {
        return;
    }

    FillRect(dc, &rect, dialog_style::backgroundBrush());

    if (active) {
        const COLORREF accentColor = dialog_style::accent();
        // 浅强调色底：把强调色以 12% 混进白底。
        const COLORREF soft = RGB(
            static_cast<int>(GetRValue(accentColor) * 0.12F + 255 * 0.88F),
            static_cast<int>(GetGValue(accentColor) * 0.12F + 255 * 0.88F),
            static_cast<int>(GetBValue(accentColor) * 0.12F + 255 * 0.88F));
        HBRUSH brush = CreateSolidBrush(soft);
        const HGDIOBJ oldBrush = SelectObject(dc, brush);
        const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RECT pill = rect;
        InflateRect(&pill, -3, -4);
        RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, 10, 10);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? dialog_style::accent()
                            : dialog_style::secondaryText());

    HFONT font =
        active && activeFont != nullptr ? activeFont : normalFont;
    HGDIOBJ previousFont =
        font != nullptr ? SelectObject(dc, font) : nullptr;

    // 文字左侧留出胶囊内边距。
    RECT textRect = rect;
    const int inset = (rect.bottom - rect.top) / 3;
    textRect.left += inset;
    textRect.right -= inset;
    DrawTextW(dc, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (previousFont != nullptr) {
        SelectObject(dc, previousFont);
    }
}

// ---------------------------------------------------------------- 消息处理

// ---------------------------------------------------------------- 跟随启动
//
// 一张「图标 + 名称 + 路径」的表 + 两个按钮。为什么是列表而不是若干复选框：
// 用户要一眼认出「加进去的是哪个程序」，靠名字容易看错（Chrome / Chrome
// Beta、各种 updater），图标是唯一可靠的辨识物；复选框做不出图标列。
//
// 数据只存在草稿里（state->followLaunch），点「确定」才随 SettingsResult
// 交回应用层 —— 与这一页其它控件同一套约定：取消 = 整份丢弃。

// 两份清单是否一致（决定要不要把结果交回应用层）。
// FollowLaunchEntry 没有 operator==：给它加一个只为这里服务，反而让
// 「两个条目什么算相等」变成 core 的公共语义，不值当。
bool sameFollowLaunch(const std::vector<core::FollowLaunchEntry>& left,
                      const std::vector<core::FollowLaunchEntry>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        // itemId 也要比：从磁贴加的条目和手动加的条目可能同名同路径，
        // 只比名字会把「把手动那条删了、换成了磁贴那条」当成没变，
        // 结果这次编辑白做（还要看来源，启动方式并不一样）。
        if (left[i].target != right[i].target ||
            left[i].name != right[i].name ||
            left[i].itemId != right[i].itemId) {
            return false;
        }
    }
    return true;
}

// 列表里显示的名字：配置里有就用，空则从路径推（去目录、去扩展名）。
std::wstring followDisplayName(const core::FollowLaunchEntry& entry) {
    if (!entry.name.empty()) {
        return base::utf8ToWideLossy(entry.name);
    }
    const std::wstring target = base::utf8ToWideLossy(entry.target);
    const std::size_t slash = target.find_last_of(L"\\/");
    std::wstring name =
        (slash == std::wstring::npos) ? target : target.substr(slash + 1);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

// 重建整张列表（含图标）。
//
// 每次都全量重建：条目最多 32 条，重建的开销远小于维护「增量更新 + 索引
// 对齐」的正确性成本（删一行之后所有 iItem 都会错位）。
void refreshFollowList(HWND dialog, SettingsDialogState* state) {
    HWND list = GetDlgItem(dialog, kFollowListId);
    if (list == nullptr || state == nullptr) {
        return;
    }
    ListView_DeleteAllItems(list);
    if (state->followIcons != nullptr) {
        ImageList_RemoveAll(state->followIcons);
    }

    for (std::size_t i = 0; i < state->followLaunch.size(); ++i) {
        const core::FollowLaunchEntry& entry = state->followLaunch[i];
        const std::wstring name = followDisplayName(entry);
        const std::wstring target = base::utf8ToWideLossy(entry.target);

        int imageIndex = -1;
        if (state->followIcons != nullptr) {
            // Small（32px）而不是 Large：清单要在 18px 高的小图里显示，
            // 从 32 缩到 18 比从 256 缩过去干净得多。
            platform::win32::UniqueHIcon icon = platform::win32::loadItemIcon(
                entry.target, std::string(), false, false,
                platform::win32::IconSizeHint::Small);
            if (icon.valid()) {
                imageIndex = ImageList_AddIcon(state->followIcons, icon.get());
            }
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | (imageIndex >= 0 ? LVIF_IMAGE : 0U);
        item.iItem = static_cast<int>(i);
        item.iImage = imageIndex >= 0 ? imageIndex : 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        const int row = ListView_InsertItem(list, &item);
        if (row >= 0) {
            ListView_SetItemText(list, row, 1,
                                 const_cast<LPWSTR>(target.c_str()));
        }
    }
}

// 选一个程序并加进草稿。返回 true 表示列表需要刷新。
bool addFollowEntry(HWND dialog, SettingsDialogState* state) {
    if (state == nullptr) {
        return false;
    }

    wchar_t file[4096] = {};
    // 过滤器里 NUL 是分隔符，文案来自语言文件，只能运行时拼
    std::wstring filter;
    filter += infrastructure::i18n::trw("filter.program");
    filter.push_back(L'\0');
    filter += L"*.exe;*.lnk;*.bat;*.cmd;*.com";
    filter.push_back(L'\0');
    filter += infrastructure::i18n::trw("filter.allFiles");
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialog;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = T("settings.followLaunch.pickTitle");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn) == FALSE) {
        return false;  // 用户取消
    }

    const std::wstring picked(file);
    core::FollowLaunchEntry entry;
    entry.target = base::wideToUtf8Lossy(picked);
    entry.name = base::wideToUtf8Lossy(followDisplayName(entry));

    // 同一条目重复添加没有意义：选中已有那行，让用户看见它在清单里。
    HWND list = GetDlgItem(dialog, kFollowListId);
    for (std::size_t i = 0; i < state->followLaunch.size(); ++i) {
        if (state->followLaunch[i].target == entry.target) {
            if (list != nullptr) {
                ListView_SetItemState(
                    list, static_cast<int>(i),
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
            }
            return false;
        }
    }

    if (state->followLaunch.size() >= core::kMaximumFollowLaunchItems) {
        const std::wstring message = infrastructure::i18n::formatW(
            "settings.followLaunch.full",
            {std::to_wstring(core::kMaximumFollowLaunchItems)});
        MessageBoxW(dialog, message.c_str(), T("settings.windowTitle"),
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }

    state->followLaunch.push_back(std::move(entry));
    return true;
}

// 删掉当前选中的一行。返回 true 表示列表需要刷新。
bool removeFollowEntry(HWND dialog, SettingsDialogState* state) {
    HWND list = GetDlgItem(dialog, kFollowListId);
    if (list == nullptr || state == nullptr) {
        return false;
    }
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0 ||
        static_cast<std::size_t>(selected) >= state->followLaunch.size()) {
        return false;
    }
    state->followLaunch.erase(state->followLaunch.begin() + selected);
    return true;
}

LRESULT CALLBACK settingsDialogProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            dialog_style::paintBackground(reinterpret_cast<HDC>(wParam), client);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc != nullptr && state != nullptr) {
                int cardHeight = kCardHeightAppearance;
                if (state->activePage == 1) {
                    cardHeight = kCardHeightBehavior;
                } else if (state->activePage == 2) {
                    cardHeight = kCardHeightPlugins;
                } else if (state->activePage == 3) {
                    cardHeight = kCardHeightRadial;
                } else if (state->activePage == 4) {
                    cardHeight = kCardHeightRadialPlugins;
                }
                RECT card{};
                card.left = scaleFor(hwnd, kContentLeft - kCardInset);
                card.top = scaleFor(hwnd, kCardTop);
                card.right =
                    card.left + scaleFor(hwnd, kContentWidth + kCardInset * 2);
                card.bottom = card.top + scaleFor(hwnd, cardHeight);
                dialog_style::paintCard(dc, card, scaleFor(hwnd, 8));
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_NCCREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
            // 同 item_editor_dialog：标题要靠 DefWindowProcW 处理
            // WM_NCCREATE 才会从 CREATESTRUCT 存进去，直接 return TRUE
            // 会让「设置」的标题栏一直空着。
            break;
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }

            HFONT font = dialogFont();
            SetPropW(hwnd, kPropFont, font);

            // 分组标题用的字体：从系统消息字体派生出「加粗、小一号」。
            // 用 GetObject 取 LOGFONT 而不是自己拼一份，DPI 与字形
            // （雅黑 / 宋体 / 高对比主题）都自动跟随系统设置。
            HFONT sectionFont = nullptr;
            if (font != nullptr) {
                LOGFONTW logFont{};
                if (GetObjectW(font, sizeof(LOGFONTW), &logFont) ==
                    static_cast<int>(sizeof(LOGFONTW))) {
                    logFont.lfWeight = FW_BOLD;
                    // lfHeight 为负表示字符高度，+1 是让它小一号
                    if (logFont.lfHeight < 0) {
                        logFont.lfHeight += 1;
                    }
                    sectionFont = CreateFontIndirectW(&logFont);
                }
            }
            SetPropW(hwnd, kPropSectionFont, sectionFont);

            // 分页标签选中态字体：同字号加粗，让「当前页」一眼可辨。
            HFONT tabFont = nullptr;
            if (font != nullptr) {
                LOGFONTW logFont{};
                if (GetObjectW(font, sizeof(LOGFONTW), &logFont) ==
                    static_cast<int>(sizeof(LOGFONTW))) {
                    logFont.lfWeight = FW_BOLD;
                    tabFont = CreateFontIndirectW(&logFont);
                }
            }
            SetPropW(hwnd, kPropTabFont, tabFont);

            // 控件归属哪个分页：创建时顺手记进对应集合，切页只做显隐。
            // 分页按钮与底部确定/取消不参与显隐，所以它们创建时 sink 为空。
            std::vector<HWND>* sink = nullptr;

            // 统一的控件创建 + 字体设置助手
            auto makeControl = [&](const wchar_t* className, const wchar_t* text,
                                   const DWORD style, const DWORD exStyle,
                                   const int x, const int y,
                                   const int w, const int h,
                                   const int id) -> HWND {
                HWND control = CreateWindowExW(
                    exStyle, className, text, style,
                    scaleFor(hwnd, x), scaleFor(hwnd, y),
                    scaleFor(hwnd, w), scaleFor(hwnd, h),
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    nullptr, nullptr);
                if (control != nullptr && font != nullptr) {
                    SendMessageW(control, WM_SETFONT,
                                 reinterpret_cast<WPARAM>(font), TRUE);
                }
                if (control != nullptr && sink != nullptr) {
                    sink->push_back(control);
                }
                return control;
            };

            auto makeLabel = [&](const wchar_t* text, const int x, const int y,
                                 const int w, const int h) -> HWND {
                return makeControl(L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, x, y, w, h, -1);
            };

            // 按当前语言实测文字宽度给按钮定宽。
            //
            // 这套布局是按中文四字按钮的像素宽手工排的：换成英文
            // （"Choose image..." / "Follow theme"）字就会被裁掉一截。
            // 这里用同一套对话框字体现量一遍，取「不小于原宽度」的结果 ——
            // 中文一个像素都不动，长语言自动撑开。
            auto measureText = [&](const wchar_t* text) -> int {
                if (text == nullptr || text[0] == L'\0' || font == nullptr) {
                    return 0;
                }
                HDC dc = GetDC(hwnd);
                if (dc == nullptr) {
                    return 0;
                }
                HGDIOBJ previous = SelectObject(dc, font);
                SIZE size{};
                const bool measured =
                    GetTextExtentPoint32W(dc, text, lstrlenW(text), &size) != FALSE;
                const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
                SelectObject(dc, previous);
                ReleaseDC(hwnd, dc);
                if (!measured || dpi <= 0) {
                    return 0;
                }
                // 量出来的是设备像素，布局写的是 96 DPI 逻辑像素，先换回来
                return MulDiv(size.cx, 96, dpi);
            };
            auto fitWidth = [&](const wchar_t* text, const int minWidth) -> int {
                // +18 = 按钮左右内边距
                return (std::max)(minWidth, measureText(text) + 18);
            };
            // 标签列宽：左对齐、右边紧跟着下拉框，只留 6px 间隔
            auto labelWidth = [&](const wchar_t* text) -> int {
                return (std::max)(kRadialLabelWidthTight, measureText(text) + 6);
            };
            // 表单标签列宽：整页所有「标签 + 控件」的行必须共用同一个宽度才
            // 对得齐，所以取当前语言里最长的那条一次算定。
            // 中文「不透明度」52px 用不完原来的 76，英文 "Interface language"
            // 要 100 出头 —— 写死就会被裁成「Interface」，右边那行也跟着错位。
            int formLabelW = kFormLabelMinWidth;
            for (const char* key : {"settings.theme",
                                    "settings.backgroundImage",
                                    "settings.opacity",
                                    "settings.tileColor",
                                    "settings.iconSize",
                                    "settings.textLabel",
                                    "settings.tabBarPosition",
                                    "settings.placement",
                                    "settings.wakeMethod",
                                    "settings.languageLabel",
                                    "settings.radial.dragThreshold",
                                    "settings.radial.cancelRadius"}) {
                formLabelW = (std::max)(formLabelW, measureText(T(key)) + 12);
            }
            // 单选框宽度：16px 圆点 + 文字 + 6px 空隙。写死 104 时中文
            // 「双击 Ctrl」刚好放下，英文 "Double-tap Ctrl" 会被裁掉一截，
            // 而且第二列还压在它身上。
            auto radioWidth = [&](const wchar_t* text) -> int {
                return measureText(text) + 26;
            };

            // ---- 左侧竖向标签栏 ----
            //
            // 自绘标签竖排在左栏：选中页画浅强调色胶囊 + 加粗文字，
            // 未选中页灰字。切页只改显隐，勾选态由 activePage 驱动重绘
            // （见 WM_DRAWITEM）。
            makeControl(L"BUTTON", T("settings.tab.appearance"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                            BS_OWNERDRAW,
                        0, kSidebarLeft, kSidebarTop, kSidebarWidth,
                        kSidebarTabHeight, kAppearanceTabId);
            makeControl(L"BUTTON", T("settings.tab.behavior"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kSidebarLeft,
                        kSidebarTop + kSidebarTabHeight + kSidebarTabGap,
                        kSidebarWidth, kSidebarTabHeight, kBehaviorTabId);
            makeControl(L"BUTTON", T("settings.tab.plugins"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kSidebarLeft,
                        kSidebarTop + (kSidebarTabHeight + kSidebarTabGap) * 2,
                        kSidebarWidth, kSidebarTabHeight, kPluginsTabId);
            makeControl(L"BUTTON", T("settings.tab.radial"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kSidebarLeft,
                        kSidebarTop + (kSidebarTabHeight + kSidebarTabGap) * 3,
                        kSidebarWidth, kSidebarTabHeight, kRadialTabId);
            makeControl(L"BUTTON", T("settings.tab.radialPlugins"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kSidebarLeft,
                        kSidebarTop + (kSidebarTabHeight + kSidebarTabGap) * 4,
                        kSidebarWidth, kSidebarTabHeight,
                        kRadialPluginsTabId);

            sink = &state->appearanceControls;
            // 单列表单：外观页的四个分组自上而下排成一列。
            // 拆页之后每页只有 1~2 个分组，不再需要并排两栏。
            int yLeft = kContentTop;

            // 分组标题：加粗小一号 + 上方细分隔线（栏首不画）。
            auto makeSection = [&](const wchar_t* text, const int id,
                                   const int x, int& y, const bool withRule) {
                if (withRule) {
                    y += 3;
                    makeControl(L"STATIC", L"",
                                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0,
                                x, y, kColumnWidth, 2, -1);
                    y += 4;
                }
                y += 2;
                HWND header = makeControl(
                    L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                    x + 8, y, kColumnWidth - 8, 18, id);
                if (header != nullptr && sectionFont != nullptr) {
                    SendMessageW(header, WM_SETFONT,
                                 reinterpret_cast<WPARAM>(sectionFont), TRUE);
                }
                y += 22;
            };

            // ==================== 外观页：背景 ====================
            //
            // 与「磁贴」分组的分工：
            //   · 背景 = 窗口这一层（皮肤、背景图、整体不透明度）；
            //   · 磁贴 = 每块磁贴自己画的底色与浓度。
            // 两者叠起来才是最终画面，所以相邻摆放，方便对照着调。

            makeSection(T("settings.section.background"), kSectionAppearanceId, kContentLeft, yLeft,
                        false);

            // 皮肤下拉：选项来自 <data>\themes\*.json（即枚举结果）。
            // 枚举为空（目录被整个删了）时退回内置预置名，保证下拉
            // 永远有得选 —— 选了不存在文件的名字，应用层会回落默认配色。
            // 皮肤决定所有基础配色（含磁贴底），所以归在「背景」里。
            makeLabel(T("settings.theme"), kContentLeft + 8, yLeft + 5, formLabelW - 8, 18);
            HWND themeCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, kContentLeft + formLabelW, yLeft,
                kComboWidth, 160,
                kThemeComboId);
            if (themeCombo != nullptr) {
                int selectIndex = -1;
                std::vector<ThemeEntry> entries = ui::enumerateThemes();
                if (entries.empty()) {
                    for (const auto& preset : ui::builtinPresets()) {
                        ThemeEntry entry;
                        entry.displayName = preset.first;
                        entries.push_back(std::move(entry));
                    }
                }
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    // 显示译文、保存规范名：切语言后已选皮肤仍然对得上
                    const std::wstring shown =
                        ui::themeDisplayName(entries[i].displayName);
                    SendMessageW(themeCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(shown.c_str()));
                    if (entries[i].displayName == state->initialThemeName) {
                        selectIndex = static_cast<int>(i);
                    }
                }
                SendMessageW(themeCombo, CB_SETCURSEL,
                             selectIndex >= 0 ? selectIndex : 0, 0);
            }
            yLeft += 26;

            // 背景图：选图 / 清除。
            // 不再显示当前文件名 —— 文件名是随机串，对用户没有信息量，
            // 反而把这一行挤得乱七八糟；设置了什么背景，主窗口一眼可见。
            makeLabel(T("settings.backgroundImage"), kContentLeft + 8, yLeft + 5, formLabelW - 8, 18);
            // 宽度按实测文字定：「选择图片...」82px 够用，「Choose image...」
            // 要 100 出头。后面的「清除」跟着右移，不写死偏移。
            const int browseWidth = fitWidth(T("settings.chooseImage"), 82);
            const int clearWidth = fitWidth(T("settings.clear"), 52);
            makeControl(
                L"BUTTON", T("settings.chooseImage"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, kContentLeft + formLabelW, yLeft, browseWidth, 23,
                kBackgroundBrowseId);
            makeControl(
                L"BUTTON", T("settings.clear"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, kContentLeft + formLabelW + browseWidth + 6, yLeft,
                clearWidth, 23, kBackgroundClearId);
            yLeft += 27;

            // ---- 不透明度（窗口）----
            //
            // 与「磁贴」分组里的不透明度不是一回事，界面上刻意分开摆：
            //   · 背景 / 不透明度 ：窗口与桌面合成时统一乘的系数（分层窗口），
            //     连搜索框、标签条、边框一起变透，透出来的是**桌面**；
            //   · 磁贴 / 不透明度 ：磁贴底色自己画多浓，透出来的是**背景图**，
            //     窗口整体仍然不透明。
            // 用滑块而不是数字框：这两项都是「边拖边看」的观感调整，
            // 输完数字才知道效果就失去意义了。
            makeLabel(T("settings.opacity"), kContentLeft + 8, yLeft + 5,
                      formLabelW - 8, 18);
            HWND windowOpacitySlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                0, kContentLeft + formLabelW, yLeft - 2, kSliderWidth, 26,
                kWindowOpacitySliderId);
            if (windowOpacitySlider != nullptr) {
                SendMessageW(windowOpacitySlider, TBM_SETRANGE, TRUE,
                             MAKELONG(50, 100));
                SendMessageW(windowOpacitySlider, TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(state->initialWindowOpacity));
            }
            {
                const std::wstring text =
                    std::to_wstring(state->initialWindowOpacity) + L"%";
                makeControl(L"STATIC", text.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                            kContentLeft + formLabelW + kSliderValueOffset,
                            yLeft + 5, 44, 18, kWindowOpacityValueId);
            }
            yLeft += 30;

            // ==================== 磁贴 ====================

            makeSection(T("settings.section.tile"), kSectionTileId, kContentLeft, yLeft, true);

            // ---- 背景色 ----
            //
            // 所有磁贴的默认背景色，优先级低于单项自己选的颜色。
            // 色块本身就是按钮（SS_NOTIFY）：点它弹内置色表，既好点也能
            // 看清当前颜色。「跟随皮肤」按钮 = 清空，回到皮肤自带的 tileFill。
            // 与下面的「不透明度」是一对：一个定色相，一个定浓度。
            makeLabel(T("settings.tileColor"), kContentLeft + 8, yLeft + 5, formLabelW - 8, 18);
            makeControl(L"STATIC", L"",
                        WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, 0,
                        kContentLeft + formLabelW, yLeft, 82, 23,
                        kDefaultTileColorSwatchId);
            makeControl(L"BUTTON", T("settings.followTheme"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, kContentLeft + formLabelW + 86, yLeft,
                        fitWidth(T("settings.followTheme"), 70), 23,
                        kDefaultTileColorClearId);
            yLeft += 30;

            // ---- 不透明度（磁贴）----
            makeLabel(T("settings.opacity"), kContentLeft + 8, yLeft + 5,
                      formLabelW - 8, 18);
            HWND tileOpacitySlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                0, kContentLeft + formLabelW, yLeft - 2, kSliderWidth, 26,
                kTileOpacitySliderId);
            if (tileOpacitySlider != nullptr) {
                SendMessageW(tileOpacitySlider, TBM_SETRANGE, TRUE,
                             MAKELONG(0, 100));
                SendMessageW(tileOpacitySlider, TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(state->initialTileOpacity));
            }
            {
                const std::wstring text =
                    std::to_wstring(state->initialTileOpacity) + L"%";
                makeControl(L"STATIC", text.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                            kContentLeft + formLabelW + kSliderValueOffset,
                            yLeft + 5, 44, 18, kTileOpacityValueId);
            }
            yLeft += 30;

            // ==================== 图标与标签 ====================

            makeSection(T("settings.section.iconLabel"), kSectionIconLabelId, kContentLeft,
                        yLeft, true);

            // ---- 图标大小 ----
            //
            // 图标相对磁贴内框的统一缩放（50~100）。格子尺寸完全不变，
            // 只改格子里的图标 —— 喜欢留白多就调小，喜欢图标大就调满。
            // 单个磁贴还可以在「修改项目」里单独覆盖这一值。
            makeLabel(T("settings.iconSize"), kContentLeft + 8, yLeft + 5,
                      formLabelW - 8, 18);
            HWND iconScaleSlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                0, kContentLeft + formLabelW, yLeft - 2, kSliderWidth, 26,
                kIconScaleSliderId);
            if (iconScaleSlider != nullptr) {
                SendMessageW(iconScaleSlider, TBM_SETRANGE, TRUE,
                             MAKELONG(50, 100));
                SendMessageW(iconScaleSlider, TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(state->initialIconScale));
            }
            {
                const std::wstring text =
                    std::to_wstring(state->initialIconScale) + L"%";
                makeControl(L"STATIC", text.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                            kContentLeft + formLabelW + kSliderValueOffset,
                            yLeft + 5, 44, 18, kIconScaleValueId);
            }
            yLeft += 30;

            // ---- 文字标签（三态）----
            //
            // 三态单选合并了原来的「总闸 + 默认行为」两层开关 ——
            // 同一件事只在一处表达，用户不必先勾总闸再选行为：
            //   · 始终显示   → showTileLabels = true， alwaysShowTileLabels = true
            //   · 移动时显示 → showTileLabels = true， alwaysShowTileLabels = false
            //   · 始终不显示 → showTileLabels = false
            // 配置字段保持不变（旧配置文件仍可直接读），只是换了呈现方式。
            makeLabel(T("settings.textLabel"), kContentLeft + 8, yLeft + 3,
                      formLabelW - 8, 18);
            {
                // 三条并排，宽度各按自己的文字实测。中文「移动时显示」与
                // 英文 "While moving" 差不多长，但德语一个词就能翻倍，
                // 写死偏移迟早会撞。
                int radioX = kContentLeft + formLabelW;
                const struct {
                    int id;
                    const wchar_t* text;
                } items[] = {
                    {kTitleAlwaysRadioId, T("settings.labelAlways")},
                    {kTitleDynamicRadioId, T("settings.labelHover")},
                    {kTitleNeverRadioId, T("settings.labelNever")},
                };
                for (const auto& item : items) {
                    const int width = radioWidth(item.text);
                    makeControl(L"BUTTON", item.text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    (item.id == kTitleAlwaysRadioId
                                         ? WS_GROUP
                                         : 0) |
                                    BS_AUTORADIOBUTTON,
                                0, radioX, yLeft, width, 20, item.id);
                    radioX += width + 14;
                }
            }
            HWND titleAlways = GetDlgItem(hwnd, kTitleAlwaysRadioId);
            HWND titleDynamic = GetDlgItem(hwnd, kTitleDynamicRadioId);
            HWND titleNever = GetDlgItem(hwnd, kTitleNeverRadioId);

            // 初值：先看总闸，再看「常驻与否」。隐藏状态下两项都勾不上。
            const bool labelsShown = state->initialShowLabels;
            const bool labelsAlwaysChosen =
                labelsShown && state->initialAlwaysShowTitle;
            if (titleAlways != nullptr) {
                SendMessageW(titleAlways, BM_SETCHECK,
                             labelsAlwaysChosen ? BST_CHECKED : BST_UNCHECKED,
                             0);
            }
            if (titleDynamic != nullptr) {
                SendMessageW(titleDynamic, BM_SETCHECK,
                             (labelsShown && !state->initialAlwaysShowTitle)
                                 ? BST_CHECKED
                                 : BST_UNCHECKED,
                             0);
            }
            if (titleNever != nullptr) {
                SendMessageW(titleNever, BM_SETCHECK,
                             labelsShown ? BST_UNCHECKED : BST_CHECKED, 0);
            }
            yLeft += 26;

            // ==================== 外观页：布局 ====================
            //
            // 磁贴格数属于「长什么样」，留在外观页；规则线把两组分开。
            makeSection(T("settings.section.layout"), kSectionLayoutId, kColumnRightX, yLeft, true);

            // 列数 / 行数并排：两者一起决定磁贴格数，拆成两行纯属浪费。
            // 上下调节器用 UDS_ALIGNRIGHT 吸附在编辑框右侧内部，
            // 所以编辑框宽度里要给箭头留出约 17px。
            // 「网格」与「行数」并排，两段的 x 都按实测文字递推：
            // 中文「网格」26px、英文 "Grid" 30 出头，写死 30 时行标签
            // 会被输入框压住；行数标签同样。
            const int gridLabelX = kColumnRightX + 8;
            const int gridLabelW = measureText(T("settings.grid")) + 6;
            makeLabel(T("settings.grid"), gridLabelX, yLeft + 3, gridLabelW, 18);
            const int columnsEditX = gridLabelX + gridLabelW;
            HWND columnsEdit = makeControl(
                L"EDIT", std::to_wstring(state->initialColumns).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                WS_EX_CLIENTEDGE,
                columnsEditX, yLeft, kSpinnerWidth, kSpinnerHeight,
                kColumnsEditId);
            SetPropW(hwnd, kPropColumnsEdit, columnsEdit);
            makeControl(
                UPDOWN_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                    UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                0, 0, 0, 0, 0, kColumnsSpinId);
            {
                HWND spin = GetDlgItem(hwnd, kColumnsSpinId);
                if (spin != nullptr) {
                    SendMessageW(spin, UDM_SETBUDDY,
                                 reinterpret_cast<WPARAM>(columnsEdit), 0);
                    SendMessageW(spin, UDM_SETRANGE32,
                                 static_cast<WPARAM>(kMinGrid),
                                 static_cast<LPARAM>(kMaxGrid));
                    SendMessageW(spin, UDM_SETPOS32, 0,
                                 static_cast<LPARAM>(state->initialColumns));
                }
            }

            const int rowsLabelX = columnsEditX + kSpinnerWidth + 18;
            const int rowsLabelW = measureText(T("settings.gridRows")) + 6;
            makeLabel(T("settings.gridRows"), rowsLabelX, yLeft + 3, rowsLabelW, 18);
            HWND rowsEdit = makeControl(
                L"EDIT", std::to_wstring(state->initialRows).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                WS_EX_CLIENTEDGE,
                rowsLabelX + rowsLabelW, yLeft, kSpinnerWidth, kSpinnerHeight,
                kRowsEditId);
            SetPropW(hwnd, kPropRowsEdit, rowsEdit);
            makeControl(
                UPDOWN_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                    UDS_SETBUDDYINT | UDS_NOTHOUSANDS,
                0, 0, 0, 0, 0, kRowsSpinId);
            {
                HWND spin = GetDlgItem(hwnd, kRowsSpinId);
                if (spin != nullptr) {
                    SendMessageW(spin, UDM_SETBUDDY,
                                 reinterpret_cast<WPARAM>(rowsEdit), 0);
                    SendMessageW(spin, UDM_SETRANGE32,
                                 static_cast<WPARAM>(kMinGrid),
                                 static_cast<LPARAM>(kMaxGrid));
                    SendMessageW(spin, UDM_SETPOS32, 0,
                                 static_cast<LPARAM>(state->initialRows));
                }
            }
            yLeft += kSpinnerHeight + 4;

            // 提示行（灰色小字）。ID 必须显式给 —— makeLabel 一律用 -1，
            // 而 WM_CTLCOLORSTATIC 靠 ID 区分「该画灰的说明文字」。
            HWND hint = makeControl(
                L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kColumnRightX + 8, yLeft, kColumnWidth - 8, 18, kHintId);
            SetPropW(hwnd, kPropHint, hint);
            yLeft += 24;

            // 标签栏位置：单选（顶部 / 底部）。
            // 纯几何平移，不重排磁贴、不改窗口尺寸，切换零风险。
            makeLabel(T("settings.tabBarPosition"), kColumnRightX + 8, yLeft + 3,
                      formLabelW - 8, 18);
            HWND tabTop = nullptr;
            HWND tabBottom = nullptr;
            {
                // 顶部 / 底部：第二个的 x 由第一个的实测宽度递推，
                // 不再写死 64px 偏移。
                int radioX = kColumnRightX + formLabelW;
                const int topWidth = radioWidth(T("settings.tabBarTop"));
                tabTop = makeControl(
                    L"BUTTON", T("settings.tabBarTop"),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                        BS_AUTORADIOBUTTON,
                    0, radioX, yLeft, topWidth, 20, kTabTopRadioId);
                radioX += topWidth + 14;
                tabBottom = makeControl(
                    L"BUTTON", T("settings.tabBarBottom"),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                    0, radioX, yLeft, radioWidth(T("settings.tabBarBottom")), 20,
                    kTabBottomRadioId);
            }
            if (tabTop != nullptr) {
                SendMessageW(tabTop, BM_SETCHECK,
                             state->initialTabBarAtBottom ? BST_UNCHECKED
                                                          : BST_CHECKED,
                             0);
            }
            if (tabBottom != nullptr) {
                SendMessageW(tabBottom, BM_SETCHECK,
                             state->initialTabBarAtBottom ? BST_CHECKED
                                                          : BST_UNCHECKED,
                             0);
            }

            // ==================== 行为页：窗口 ====================
            //
            // 「外观」答的是长什么样，「行为」答的是在哪儿、怎么出现。
            // 两页各自从内容区顶部开始，切换只改显隐。
            sink = &state->behaviorControls;
            int yRight = kContentTop;

            // ==================== 行为页：语言 ====================
            //
            // 语言放行为页最上面：它不改变外观，只决定「这一整套界面用哪套
            // 文案」，属于应用级行为。切换后立刻落盘并重取词，设置窗口自己
            // 在下次打开时变（对话框已建好的控件不会自己改字）。
            makeSection(T("settings.language"), kSectionLanguageId,
                        kColumnRightX, yRight, false);
            makeLabel(T("settings.languageLabel"), kColumnRightX + 8,
                      yRight + 5, formLabelW - 8, 18);
            {
                HWND languageCombo = makeControl(
                    L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    0, kColumnRightX + formLabelW, yRight, kComboWidth, 200,
                    kLanguageComboId);
                fillLanguageCombo(languageCombo, state->initialLanguage);
            }
            yRight += 26;
            makeLabel(T("settings.language.hint"), kColumnRightX + 8, yRight,
                      kColumnWidth - 8, 18);
            yRight += 24;

            makeSection(T("settings.section.window"), kSectionWindowId, kColumnRightX, yRight,
                        false);

            // 出现位置：下拉。展示顺序与 core::WindowPlacement 的枚举值
            // 刻意不同（枚举要保已存配置不错位，Center 只能追加末尾），
            // 取值时经 kPlacementOptions 映射，不拿 CB_GETCURSEL 当枚举用。
            makeLabel(T("settings.placement"), kColumnRightX + 8, yRight + 5,
                      formLabelW - 8, 18);
            HWND placementCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, kColumnRightX + formLabelW, yRight,
                kComboWidth, 160,
                kWindowPlacementComboId);
            if (placementCombo != nullptr) {
                for (int i = 0; i < kPlacementOptionCount; ++i) {
                    SendMessageW(placementCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(
                                     T(kPlacementOptions[i].key)));
                }
                int index = 0;
                for (int i = 0; i < kPlacementOptionCount; ++i) {
                    if (kPlacementOptions[i].value ==
                        state->initialWindowPlacement) {
                        index = i;
                        break;
                    }
                }
                SendMessageW(placementCombo, CB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
            }
            yRight += 26;

            // ==================== 启动与唤醒 ====================

            makeSection(T("settings.section.startup"), kSectionStartupId, kColumnRightX,
                        yRight, true);

            HWND startup = makeControl(
                L"BUTTON", T("settings.startWithWindows"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                    BS_AUTOCHECKBOX,
                0, kColumnRightX + 8, yRight, kColumnWidth - 8, 20,
                kStartupCheckId);
            if (startup != nullptr) {
                SendMessageW(startup, BM_SETCHECK,
                             state->initialStartup ? BST_CHECKED : BST_UNCHECKED,
                             0);
            }
            yRight += 26;

            // ---- 跟随启动 ----
            //
            // 与「开机自动启动」是一件事的两半：前者决定 KTile 自己起不起来，
            // 后者决定起来之后顺手把哪些程序带起来。所以紧挨着放。
            //
            // 用列表而不是一堆复选框：一眼要认出「加进去的是哪个程序」，
            // 靠名字容易看错（Chrome / Chrome Beta、各种 updater），
            // 图标才是可靠的辨识物。
            {
                HWND followLabel = makeLabel(
                    T("settings.followLaunch"), kColumnRightX + 8, yRight + 4,
                    formLabelW - 8, 18);
                addTooltip(hwnd, followLabel, T("settings.followLaunch.tip"));

                // 按钮贴右边界，宽度按实测文字定（"Remove" 比「移除」宽，
                // 写死 72 会把英文裁掉）。从右往左推，右边缘永远是齐的。
                int buttonRight = kColumnRightX + kColumnWidth - 8;
                const int removeWidth =
                    fitWidth(T("settings.followLaunch.remove"), 72);
                buttonRight -= removeWidth;
                makeControl(L"BUTTON", T("settings.followLaunch.remove"),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            0, buttonRight, yRight, removeWidth, kButtonHeight,
                            kFollowRemoveId);
                buttonRight -= 8;
                const int addWidth =
                    fitWidth(T("settings.followLaunch.add"), 72);
                buttonRight -= addWidth;
                makeControl(L"BUTTON", T("settings.followLaunch.add"),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            0, buttonRight, yRight, addWidth, kButtonHeight,
                            kFollowAddId);
            }
            yRight += 30;

            {
                HWND followList = makeControl(
                    WC_LISTVIEWW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                        LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                    WS_EX_CLIENTEDGE, kColumnRightX + 8, yRight,
                    kColumnWidth - 8, kFollowListHeight, kFollowListId);
                if (followList != nullptr) {
                    ListView_SetExtendedListViewStyle(
                        followList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

                    // 图标表与列表同生命周期：列表持有它，销毁时一起回收
                    // （WM_DESTROY 里 Destroy）。
                    state->followIcons = ImageList_Create(
                        scaleFor(hwnd, kFollowIconSize),
                        scaleFor(hwnd, kFollowIconSize), ILC_COLOR32 | ILC_MASK,
                        1, static_cast<int>(core::kMaximumFollowLaunchItems));
                    if (state->followIcons != nullptr) {
                        ListView_SetImageList(followList, state->followIcons,
                                              LVSIL_SMALL);
                    }

                    // 名称列宽实测（英文 "Name" 与中文「名称」差不了几像素，
                    // 但留出的余量要跟着字体走），路径列吃掉剩下的宽度。
                    const int nameWidth =
                        (std::max)(140, measureText(T("common.name")) + 40);
                    LVCOLUMNW nameColumn{};
                    nameColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    nameColumn.pszText = const_cast<LPWSTR>(T("common.name"));
                    nameColumn.cx = scaleFor(hwnd, nameWidth);
                    nameColumn.fmt = LVCFMT_LEFT;
                    ListView_InsertColumn(followList, 0, &nameColumn);

                    LVCOLUMNW pathColumn{};
                    pathColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    pathColumn.pszText =
                        const_cast<LPWSTR>(T("settings.followLaunch.col.path"));
                    pathColumn.cx = scaleFor(
                        hwnd, kColumnWidth - 8 - nameWidth - 3);
                    pathColumn.fmt = LVCFMT_LEFT;
                    ListView_InsertColumn(followList, 1, &pathColumn);

                    addTooltip(hwnd, followList,
                               T("settings.followLaunch.tip"));
                }
                yRight += kFollowListHeight + 10;
            }

            // 唤醒快捷键（2x2 单选）：
            // 前两项是系统级注册热键（零开销），后两项是手势，需要装
            // 低级键盘钩子（RegisterHotKey 表达不了双击/单键）。
            // 做成预设而不是「录制组合键」：录制要处理修饰键、冲突检测、
            // 反馈文案，收益却很低 —— 这四个已经覆盖绝大多数诉求。
            makeLabel(T("settings.wakeMethod"), kColumnRightX + 8, yRight + 3,
                      formLabelW - 8, 18);
            {
                struct PresetRadio {
                    int id;
                    const wchar_t* label;
                    core::HotkeyPreset preset;
                    int column;  // 0 = 左列，1 = 右列
                    int y;
                };
                const PresetRadio radios[] = {
                    {kHotkeyAltSpaceRadioId, L"Alt+Space",
                     core::HotkeyPreset::AltSpace, 0, yRight},
                    {kHotkeyCtrlSpaceRadioId, L"Ctrl+Space",
                     core::HotkeyPreset::CtrlSpace, 1, yRight},
                    {kHotkeyDoubleCtrlRadioId, T("settings.hotkey.doubleCtrl"),
                     core::HotkeyPreset::DoubleCtrl, 0, yRight + 22},
                    {kHotkeyWinKeyRadioId, T("settings.hotkey.winKey"),
                     core::HotkeyPreset::WinKey, 1, yRight + 22},
                };
                // 列宽取该列最长的一条（实测），右列紧跟左列最宽的那条：
                // 中文「双击 Ctrl」与英文 "Double-tap Ctrl" 差 40 多像素，
                // 写死 104 宽 + 98 偏移时英文尾巴被裁，还压到右边那列上。
                const int columnGap = 14;
                const int columnX[2] = {
                    kColumnRightX + formLabelW,
                    kColumnRightX + formLabelW + columnGap +
                        (std::max)(radioWidth(radios[0].label),
                                   radioWidth(radios[2].label))};
                for (const auto& radio : radios) {
                    const bool checked =
                        radio.preset == state->initialHotkeyPreset;
                    HWND button = makeControl(
                        L"BUTTON", radio.label,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                            (radio.id == kHotkeyAltSpaceRadioId ? WS_GROUP : 0) |
                            BS_AUTORADIOBUTTON,
                        0, columnX[radio.column], radio.y,
                        radioWidth(radio.label), 20, radio.id);
                    if (button != nullptr) {
                        SendMessageW(button, BM_SETCHECK,
                                     checked ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                }
                // 单按 Win 会被我们接管，所以把「Ctrl+Win 仍开开始菜单」
                // 这条补偿写在界面上，否则用户会以为开始菜单坏了。
                // 与上一行留 6px：原来只差 2px，两行几乎糊在一起。
                makeLabel(T("settings.winKeyNote"),
                          kColumnRightX + 8, yRight + 48,
                          kColumnWidth - 8, 18);
            }
            yRight += 66;

            // ==================== 插件管理页 ====================
            //
            // 这一页不读写任何配置，只管理磁盘上的插件目录：
            //   · 安装 = 把用户选中的 plugin.json 所在文件夹整体复制进来；
            //   · 删除 = 移除整个插件目录；
            //   · 添加磁贴 = 把插件挂到当前标签页。
            // 文件操作全部经应用层回调，UI 层不碰磁盘。
            sink = &state->pluginControls;

            // 一句话说明插件目录的位置；具体路径太长，放不下就不硬塞，
            // 用户点「打开目录」能直接看到真实位置。
            makeLabel(T("settings.plugins.intro"),
                      kContentLeft, kPluginIntroTop, kContentWidth,
                      kPluginIntroHeight);

            HWND pluginList = makeControl(
                WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                    LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                WS_EX_CLIENTEDGE,
                kContentLeft, kPluginListTop, kContentWidth,
                kPluginListHeight, kPluginListId);
            if (pluginList != nullptr) {
                // 整行选中 + 双缓冲：列表只有几行，但拖动滚动条时
                // 双缓冲能彻底消掉 Win7 上偶发的白条闪烁。
                ListView_SetExtendedListViewStyle(
                    pluginList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

                struct ColumnSpec {
                    const wchar_t* text;
                    int width;
                };
                const ColumnSpec columns[] = {
                    {T("common.name"), 130}, {L"ID", 170}, {T("settings.plugins.col.source"), 76},
                    {T("settings.plugins.col.refresh"), 76}, {T("settings.plugins.col.visible"), 70},
                };
                const int columnCount =
                    static_cast<int>(sizeof(columns) / sizeof(columns[0]));
                for (int i = 0; i < columnCount; ++i) {
                    LVCOLUMNW column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    column.pszText = const_cast<LPWSTR>(columns[i].text);
                    column.cx = scaleFor(hwnd, columns[i].width);
                    column.fmt = LVCFMT_LEFT;
                    ListView_InsertColumn(pluginList, i, &column);
                }
                SetPropW(hwnd, kPropPluginList, pluginList);
            }

            // 操作按钮一行排开。宽度按实测文字定、x 跟着前一个递推：
            // 中文四个字 96px 够用，英文 "Install plugin..." 要 110 出头，
            // 写死会把字裁成「Install plugin.」。
            {
                const struct {
                    const char* key;
                    int id;
                } buttons[] = {
                    {"settings.plugins.install", kPluginInstallId},
                    {"settings.plugins.addTile", kPluginAddTileId},
                    {"settings.plugins.openFolder", kPluginOpenFolderId},
                    {"settings.plugins.remove", kPluginRemoveId},
                };
                int buttonX = kContentLeft;
                for (const auto& button : buttons) {
                    const int width =
                        fitWidth(T(button.key), kPluginButtonWidth);
                    makeControl(L"BUTTON", T(button.key),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_OWNERDRAW,
                                0, buttonX, kPluginButtonTop, width,
                                kButtonHeight, button.id);
                    buttonX += width + 8;
                }
            }

            makeControl(
                L"STATIC", T("settings.plugins.footnote"),
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kContentLeft, kPluginHintTop, kContentWidth,
                kPluginHintHeight, kPluginHintId);

            // ==================== 轮盘菜单页 ====================
            //
            // 这一页只放「什么时候呼出」；扇区动作与按应用匹配在
            // radial_editor_dialog 里编辑（列表 + 表单，塞不进单列表单）。
            //
            // 版式原则：控件上只写名字，解释全部挂 tooltip。成段的灰字说明读起来
            // 像说明书，还会把「这一行该改哪个控件」冲淡 —— 想看解释的人停一下
            // 鼠标就有，不想看的人眼前干净。
            sink = &state->radialControls;
            int yRadial = kContentTop;

            makeSection(T("settings.section.trigger"), kSectionRadialTriggerId, kContentLeft,
                        yRadial, false);

            HWND radialEnabled = makeControl(
                L"BUTTON", T("settings.radial.enable"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
                0, kContentLeft + 8, yRadial, kColumnWidth - 8, 20,
                kRadialEnabledCheckId);
            if (radialEnabled != nullptr) {
                SendMessageW(radialEnabled, BM_SETCHECK,
                             state->radial.trigger.enabled ? BST_CHECKED
                                                           : BST_UNCHECKED,
                             0);
            }
            yRadial += 28;

            // ---- 一行三组：触发键 / 修饰键 / 抑制键 ----
            //
            // 三者描述同一件事（按住什么键、还要按下什么、什么情况下让路），
            // 拆成两三行就看不出这层关系。列位置按实测标签宽度依次排下来，
            // 见 kRadialLabelWidthTight 一带的注释。
            // 两个下拉的宽度都按各自选项实测（中文「右键拖动」52px 用不完
            // 84，英文 "Middle-drag" 要 100 出头）。
            int triggerComboW = kRadialComboMinWidth;
            for (int i = 0; i < kRadialButtonOptionCount; ++i) {
                triggerComboW = (std::max)(
                    triggerComboW,
                    measureText(T(kRadialButtonOptions[i].key)) + kComboExtraWidth);
            }
            int modifierComboW = kRadialComboMinWidth;
            for (int i = 0; i < kRadialModifierOptionCount; ++i) {
                modifierComboW = (std::max)(
                    modifierComboW,
                    measureText(T(kRadialModifierOptions[i].key)) + kComboExtraWidth);
            }
            int triggerX = kContentLeft + 8;
            const int triggerLabelW = labelWidth(T("settings.radial.triggerKey"));
            makeLabel(T("settings.radial.triggerKey"), triggerX, yRadial + 4,
                      triggerLabelW, 18);
            triggerX += triggerLabelW;
            HWND radialButtonCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                triggerX, yRadial, triggerComboW, 160,
                kRadialButtonComboId);
            if (radialButtonCombo != nullptr) {
                int index = 0;
                for (int i = 0; i < kRadialButtonOptionCount; ++i) {
                    SendMessageW(radialButtonCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(
                                     T(kRadialButtonOptions[i].key)));
                    if (kRadialButtonOptions[i].value ==
                        state->radial.trigger.button) {
                        index = i;
                    }
                }
                SendMessageW(radialButtonCombo, CB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
            }
            triggerX += triggerComboW + 6;

            const int modifierLabelW = labelWidth(T("settings.radial.modifier"));
            makeLabel(T("settings.radial.modifier"), triggerX, yRadial + 4,
                      modifierLabelW, 18);
            triggerX += modifierLabelW;
            HWND radialModifierCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
                triggerX, yRadial, modifierComboW, 160,
                kRadialModifierComboId);
            if (radialModifierCombo != nullptr) {
                int index = 0;
                for (int i = 0; i < kRadialModifierOptionCount; ++i) {
                    SendMessageW(radialModifierCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(
                                     T(kRadialModifierOptions[i].key)));
                    if (kRadialModifierOptions[i].value ==
                        state->radial.trigger.modifier) {
                        index = i;
                    }
                }
                SendMessageW(radialModifierCombo, CB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
            }
            triggerX += modifierComboW + 6;

            // 抑制键：按住这些键时轮盘一律不弹，鼠标事件原样放行。
            // 一个可勾选的下拉（自绘 + 列表子类化，见 suppressListProc）。
            // 标签带 ID：选修饰键时它要跟着下拉一起变灰。
            const int suppressLabelW = labelWidth(T("settings.radial.suppress"));
            makeControl(L"STATIC", T("settings.radial.suppress"), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                        triggerX, yRadial + 3,
                        suppressLabelW, 18, kRadialSuppressLabelId);
            triggerX += suppressLabelW;
            // 抑制键下拉按内容定宽。
            //
            // 之前让它吃掉行尾的剩余宽度（182px）撑满整行，视觉上像是把
            // 整个设置界面横向拉开了一截：框里只写「Ctrl」两个字，剩下
            // 一大片空白，还逼着用户把鼠标多移一段。选项只有
            // Ctrl / Shift / Alt / Win，量一下最长的那条 + 勾选框 + 箭头
            // 就够了，右侧留白比空框好看。
            int suppressOptionW = 0;
            for (int i = 0; i < kRadialSuppressOptionCount; ++i) {
                suppressOptionW = (std::max)(
                    suppressOptionW,
                    measureText(kRadialSuppressOptions[i].label));
            }
            const int suppressComboW =
                (std::max)(96, suppressOptionW + 20 /* 勾选框 */ + 22 /* 箭头 */);
            HWND suppressCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                    CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 0,
                triggerX, yRadial, suppressComboW, 160,
                kRadialSuppressComboId);
            initSuppressCombo(suppressCombo, state->radial);
            yRadial += 30;

            // 拖动阈值：超过这个距离才认为「用户是在做手势」而不是单击。
            makeLabel(T("settings.radial.dragThreshold"), kContentLeft + 8, yRadial + 5,
                      formLabelW - 8, 18);
            HWND thresholdSlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0,
                kContentLeft + formLabelW, yRadial - 2, kSliderWidth, 26,
                kRadialThresholdSliderId);
            if (thresholdSlider != nullptr) {
                SendMessageW(thresholdSlider, TBM_SETRANGE, TRUE,
                             MAKELONG(kRadialThresholdMin, kRadialThresholdMax));
                SendMessageW(thresholdSlider, TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(
                                 state->radial.trigger.activationThresholdPx));
            }
            {
                const std::wstring text =
                    std::to_wstring(state->radial.trigger.activationThresholdPx) +
                    infrastructure::i18n::trw("unit.pixels");
                makeControl(L"STATIC", text.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                            kContentLeft + formLabelW + kSliderValueOffset,
                            yRadial + 4, 90, 18, kRadialThresholdValueId);
            }
            yRadial += 30;

            // 外甩取消半径：拖出这个半径再松手 = 取消，不触发任何动作。
            makeLabel(T("settings.radial.cancelRadius"), kContentLeft + 8, yRadial + 5,
                      formLabelW - 8, 18);
            HWND cancelSlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0,
                kContentLeft + formLabelW, yRadial - 2, kSliderWidth, 26,
                kRadialCancelSliderId);
            if (cancelSlider != nullptr) {
                SendMessageW(cancelSlider, TBM_SETRANGE, TRUE,
                             MAKELONG(kRadialCancelMin, kRadialCancelMax));
                SendMessageW(cancelSlider, TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(
                                 state->radial.trigger.cancelRadiusPx));
            }
            {
                const std::wstring text =
                    std::to_wstring(state->radial.trigger.cancelRadiusPx) +
                    infrastructure::i18n::trw("unit.pixels");
                makeControl(L"STATIC", text.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                            kContentLeft + formLabelW + kSliderValueOffset,
                            yRadial + 4, 90, 18, kRadialCancelValueId);
            }
            yRadial += 28;

            makeSection(T("settings.section.sectors"), kSectionRadialSectorId, kContentLeft,
                        yRadial, true);

            HWND radialSummary = makeControl(
                L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kContentLeft + 8, yRadial, kColumnWidth - 8, 18,
                kRadialSummaryId);
            yRadial += 24;

            // 宽度按实测文字定：「编辑扇区动作...」118px 够，英文
            // "Edit sector actions…" 要 130 出头，写死会把省略号裁掉。
            makeControl(L"BUTTON", T("settings.radial.edit"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kContentLeft + 8, yRadial,
                        fitWidth(T("settings.radial.edit"), 118), 26,
                        kRadialEditButtonId);

            // ---- 悬停提示 ----
            //
            // 这些正是原先印在页面上的那几段灰字。措辞基本照搬，只是从
            // 「一直占着版面」改成「想知道才看」。
            addTooltip(hwnd, radialEnabled, T("settings.radial.tip.enable"));
            addTooltip(hwnd, radialButtonCombo, T("settings.radial.tip.triggerKey"));
            addTooltip(hwnd, radialModifierCombo, T("settings.radial.tip.modifier"));
            addTooltip(hwnd, thresholdSlider, T("settings.radial.tip.dragThreshold"));
            addTooltip(hwnd, cancelSlider, T("settings.radial.tip.cancelRadius"));
            addTooltip(hwnd, suppressCombo, T("settings.radial.tip.suppress"));
            addTooltip(hwnd, radialSummary, T("settings.radial.tip.summary"));

            // 初始可用性：修饰键若是 Ctrl / Shift / Alt，抑制键一进来就该是灰的
            refreshRadialSuppressEnabled(hwnd);

            // ==================== 轮盘插件页 ====================
            //
            // 这一页是**只读清单**。轮盘插件天生是「一个目录 + 一份 JSON +
            // 一堆图标」，做成表格编辑器就等于把它退化成「只能在界面里配」，
            // 「加个目录就是加个应用」的好处全没了。所以这里只做三件事：
            // 看清加载了哪些、看清某个为什么被跳过、一键打开那份 JSON。
            sink = &state->radialPluginControls;

            makeLabel(T("settings.radialPlugins.intro"),
                      kContentLeft, kPluginIntroTop, kContentWidth,
                      kPluginIntroHeight);

            HWND radialPluginList = makeControl(
                WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                    LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                WS_EX_CLIENTEDGE,
                kContentLeft, kPluginListTop, kContentWidth,
                kPluginListHeight, kRadialPluginListId);
            if (radialPluginList != nullptr) {
                // INFOTIP：跳过原因往往比「说明」列宽，悬停能看到全文 ——
                // 这一列存在的意义就是解释「为什么没加载」，被截断就等于没说。
                ListView_SetExtendedListViewStyle(
                    radialPluginList, LVS_EX_FULLROWSELECT |
                                          LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);

                struct RadialColumnSpec {
                    const wchar_t* text;
                    int width;
                };
                const RadialColumnSpec columns[] = {
                    {T("settings.radialPlugins.col.status"), 58}, {T("common.name"), 108}, {T("settings.radialPlugins.col.processes"), 140},
                    {T("settings.radialPlugins.col.sectors"), 82}, {T("settings.radialPlugins.col.note"), 150},
                };
                const int columnCount =
                    static_cast<int>(sizeof(columns) / sizeof(columns[0]));
                for (int i = 0; i < columnCount; ++i) {
                    LVCOLUMNW column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    column.pszText = const_cast<LPWSTR>(columns[i].text);
                    column.cx = scaleFor(hwnd, columns[i].width);
                    column.fmt = LVCFMT_LEFT;
                    ListView_InsertColumn(radialPluginList, i, &column);
                }
                SetPropW(hwnd, kPropRadialPluginList, radialPluginList);
            }

            {
                const struct {
                    const char* key;
                    int id;
                } buttons[] = {
                    {"settings.radialPlugins.openJson", kRadialPluginOpenJsonId},
                    {"settings.plugins.openFolder", kRadialPluginOpenFolderId},
                    {"settings.radialPlugins.rescan", kRadialPluginRescanId},
                };
                int buttonX = kContentLeft;
                for (const auto& button : buttons) {
                    const int width =
                        fitWidth(T(button.key), kPluginButtonWidth);
                    makeControl(L"BUTTON", T(button.key),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_OWNERDRAW,
                                0, buttonX, kPluginButtonTop, width,
                                kButtonHeight, button.id);
                    buttonX += width + 8;
                }
            }

            makeControl(
                L"STATIC", T("settings.radialPlugins.footnote"),
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kContentLeft, kPluginHintTop, kContentWidth,
                kRadialPluginHintHeight, -1);

            // 行为页与插件页的控件先全部隐藏，启动时停在外观页
            for (const HWND control : state->behaviorControls) {
                ShowWindow(control, SW_HIDE);
            }
            for (const HWND control : state->pluginControls) {
                ShowWindow(control, SW_HIDE);
            }
            for (const HWND control : state->radialControls) {
                ShowWindow(control, SW_HIDE);
            }
            for (const HWND control : state->radialPluginControls) {
                ShowWindow(control, SW_HIDE);
            }

            // ---- 底部按钮（贴右下角） ----
            // 确定/取消在两个分页下都在，因此不加入任何一页的集合。
            sink = nullptr;
            const int buttonY = kDialogHeight - kMargin - kButtonHeight;
            makeControl(
                L"BUTTON", T("common.ok"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                kDialogWidth - kMargin - 2 * kButtonWidth - 8, buttonY,
                kButtonWidth, kButtonHeight, kOkId);
            makeControl(
                L"BUTTON", T("common.cancel"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                kDialogWidth - kMargin - kButtonWidth, buttonY,
                kButtonWidth, kButtonHeight, kCancelId);

            refreshHint(hwnd, state);
            refreshRadialSummary(hwnd, state);
            refreshFollowList(hwnd, state);

            if (columnsEdit != nullptr) {
                SetFocus(columnsEdit);
                SendMessageW(columnsEdit, EM_SETSEL, 0, -1);
            }
            return 0;
        }

        case WM_NOTIFY: {
            // 插件列表的两类通知：选中变化（决定按钮可用性）与双击行
            // （快捷添加到当前标签页）。
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr || state == nullptr) {
                break;
            }
            // 轮盘插件列表：只有选中变化一件事，且双击 = 直接打开那份 JSON
            // （这一页的动作几乎都是「我要去改它」，双击打开最省一步）。
            if (header->idFrom == static_cast<UINT>(kRadialPluginListId)) {
                if (header->code == LVN_ITEMCHANGED) {
                    const std::wstring id = selectedRadialPluginId(hwnd, state);
                    const BOOL enable = id.empty() ? FALSE : TRUE;
                    EnableWindow(GetDlgItem(hwnd, kRadialPluginOpenJsonId),
                                 enable);
                    EnableWindow(GetDlgItem(hwnd, kRadialPluginOpenFolderId),
                                 enable);
                    return 0;
                }
                if (header->code == NM_DBLCLK) {
                    const std::wstring id = selectedRadialPluginId(hwnd, state);
                    if (!id.empty() && state->radialPlugins != nullptr &&
                        state->radialPlugins->openManifest) {
                        state->radialPlugins->openManifest(
                            base::wideToUtf8Lossy(id));
                    }
                    return 0;
                }
                break;
            }
            if (header->idFrom != static_cast<UINT>(kPluginListId)) {
                break;
            }
            HWND list = static_cast<HWND>(GetPropW(hwnd, kPropPluginList));
            if (list == nullptr) {
                break;
            }
            if (header->code == LVN_ITEMCHANGED) {
                const int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                const bool hasSelection =
                    index >= 0 &&
                    static_cast<std::size_t>(index) < state->pluginRows.size();
                EnableWindow(GetDlgItem(hwnd, kPluginAddTileId),
                             hasSelection ? TRUE : FALSE);
                EnableWindow(GetDlgItem(hwnd, kPluginRemoveId),
                             hasSelection ? TRUE : FALSE);
                return 0;
            }
            if (header->code == NM_DBLCLK) {
                const std::wstring id = selectedPluginId(hwnd, state);
                if (!id.empty() && state->plugins != nullptr &&
                    state->plugins->addTile) {
                    state->plugins->addTile(base::wideToUtf8Lossy(id));
                    MessageBoxW(hwnd, T("settings.plugins.added"), T("settings.tab.plugins"),
                                MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }
            break;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);

            // 分页标签：按 activePage 决定谁画选中态。
            if (draw != nullptr && draw->CtlType == ODT_BUTTON &&
                (draw->CtlID == static_cast<UINT>(kAppearanceTabId) ||
                 draw->CtlID == static_cast<UINT>(kBehaviorTabId) ||
                 draw->CtlID == static_cast<UINT>(kPluginsTabId) ||
                 draw->CtlID == static_cast<UINT>(kRadialTabId) ||
                 draw->CtlID == static_cast<UINT>(kRadialPluginsTabId))) {
                wchar_t text[64] = {};
                GetWindowTextW(draw->hwndItem, text, 64);
                int tabPage = 2;
                if (draw->CtlID == static_cast<UINT>(kAppearanceTabId)) {
                    tabPage = 0;
                } else if (draw->CtlID == static_cast<UINT>(kBehaviorTabId)) {
                    tabPage = 1;
                } else if (draw->CtlID == static_cast<UINT>(kRadialTabId)) {
                    tabPage = 3;
                } else if (draw->CtlID ==
                           static_cast<UINT>(kRadialPluginsTabId)) {
                    tabPage = 4;
                }
                const bool active =
                    state != nullptr && state->activePage == tabPage;
                paintSettingsTab(
                    draw->hDC, draw->rcItem, text, active,
                    static_cast<HFONT>(GetPropW(hwnd, kPropFont)),
                    static_cast<HFONT>(GetPropW(hwnd, kPropTabFont)));
                // 鼠标点击后系统会置 ODS_NOFOCUSRECT，此时不画虚线框；
                // 只有键盘 Tab 过来才画，和原生按钮的行为一致。
                if ((draw->itemState & ODS_FOCUS) != 0 &&
                    (draw->itemState & ODS_NOFOCUSRECT) == 0) {
                    RECT focus = draw->rcItem;
                    InflateRect(&focus, -2, -2);
                    DrawFocusRect(draw->hDC, &focus);
                }
                return TRUE;
            }

            // 抑制键下拉：字段（itemID == -1）画汇总，列表行画勾选方格。
            // 为什么自绘：「当前选中项」这个概念在这里不成立（是 N 选多），
            // 原生下拉只能显示选中项的文字，收起后看不出勾了哪几个。
            if (draw != nullptr && draw->CtlType == ODT_COMBOBOX &&
                draw->CtlID == static_cast<UINT>(kRadialSuppressComboId)) {
                const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
                HFONT comboFont = static_cast<HFONT>(GetPropW(hwnd, kPropFont));
                if (draw->itemID == static_cast<UINT>(-1)) {
                    paintSuppressField(draw->hDC, draw->rcItem, comboFont,
                                       suppressSummary(draw->hwndItem),
                                       disabled);
                } else {
                    const int index = static_cast<int>(draw->itemID);
                    if (index >= 0 && index < kRadialSuppressOptionCount) {
                        paintSuppressRow(
                            draw->hDC, draw->rcItem, comboFont,
                            kRadialSuppressOptions[index].label,
                            suppressChecked(draw->hwndItem, index),
                            (draw->itemState & ODS_SELECTED) != 0, disabled);
                    }
                }
                return TRUE;
            }

            // 自绘控件只有「磁贴背景色」样本一块。
            // SS_OWNERDRAW 的 STATIC 在 WM_DRAWITEM 里的 CtlType 是
            // ODT_STATIC（不是 ODT_BUTTON），按 CtlID 判定即可。
            if (draw != nullptr && draw->CtlType == ODT_STATIC &&
                draw->CtlID == static_cast<UINT>(kDefaultTileColorSwatchId)) {
                paintColorSwatch(
                    draw->hDC, draw->rcItem,
                    state != nullptr
                        ? state->chosenDefaultTileColor
                        : std::optional<std::uint32_t>());
                return TRUE;
            }

            // 其余 BS_OWNERDRAW 按钮：确定用强调色主按钮，其余白底细边。
            if (draw != nullptr && draw->CtlType == ODT_BUTTON) {
                wchar_t text[64] = {};
                GetWindowTextW(draw->hwndItem, text, 64);
                dialog_style::paintButton(
                    draw->hDC, draw->rcItem, text,
                    static_cast<HFONT>(GetPropW(hwnd, kPropFont)),
                    draw->CtlID == static_cast<UINT>(kOkId)
                        ? dialog_style::ButtonKind::Primary
                        : dialog_style::ButtonKind::Secondary,
                    (draw->itemState & ODS_SELECTED) != 0,
                    (draw->itemState & ODS_DISABLED) != 0,
                    (draw->itemState & ODS_FOCUS) != 0 &&
                        (draw->itemState & ODS_NOFOCUSRECT) == 0);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int controlId = LOWORD(wParam);
            const int notification = HIWORD(wParam);

            // IsDialogMessageW 对普通窗口的回车 / Esc 固定发 IDOK / IDCANCEL，
            // 而本对话框的确定 / 取消用的是自有 ID（1130 / 1131）。
            // 不映射的话回车与 Esc 都落空 —— 尤其按钮改成 owner-draw 之后
            // 系统也不再靠 BS_DEFPUSHBUTTON 推断默认按钮。
            if (controlId == IDOK) {
                controlId = kOkId;
            } else if (controlId == IDCANCEL) {
                controlId = kCancelId;
            }

            // ---- 分页切换 ----
            if ((controlId == kAppearanceTabId ||
                 controlId == kBehaviorTabId ||
                 controlId == kPluginsTabId ||
                 controlId == kRadialTabId ||
                 controlId == kRadialPluginsTabId) &&
                notification == BN_CLICKED) {
                int page = 2;
                if (controlId == kAppearanceTabId) {
                    page = 0;
                } else if (controlId == kBehaviorTabId) {
                    page = 1;
                } else if (controlId == kRadialTabId) {
                    page = 3;
                } else if (controlId == kRadialPluginsTabId) {
                    page = 4;
                }
                switchSettingsPage(hwnd, state, page);
                return 0;
            }

            // ---- 轮盘插件页：只读，动作全部转交应用层 ----
            if ((controlId == kRadialPluginOpenJsonId ||
                 controlId == kRadialPluginOpenFolderId ||
                 controlId == kRadialPluginRescanId) &&
                notification == BN_CLICKED) {
                if (state == nullptr || state->radialPlugins == nullptr) {
                    return 0;
                }
                const SettingsRadialPluginHooks& hooks = *state->radialPlugins;
                if (controlId == kRadialPluginRescanId) {
                    if (hooks.rescan) {
                        hooks.rescan();
                    }
                    refreshRadialPluginList(hwnd, state);
                    return 0;
                }
                const std::string id =
                    base::wideToUtf8Lossy(selectedRadialPluginId(hwnd, state));
                if (id.empty()) {
                    return 0;  // 按钮本该是灰的，走到这里说明状态刚变，忽略
                }
                if (controlId == kRadialPluginOpenJsonId) {
                    if (hooks.openManifest) {
                        hooks.openManifest(id);
                    }
                } else if (hooks.openFolder) {
                    hooks.openFolder(id);
                }
                return 0;
            }

            // ---- 轮盘：修饰键变化决定抑制键是否可用 ----
            //
            // 只在选中项真的变了时更新。这里不改 state（草稿），因为
            // 保存时统一从控件读；回调只负责把界面调成「能表达的样子」。
            if (controlId == kRadialModifierComboId &&
                notification == CBN_SELCHANGE) {
                refreshRadialSuppressEnabled(hwnd);
                return 0;
            }

            // ---- 跟随启动：添加 / 移除 ----
            //
            // 只改草稿，点确定才落盘（与这一页其它控件一致）。
            if (controlId == kFollowAddId && notification == BN_CLICKED) {
                if (state != nullptr && addFollowEntry(hwnd, state)) {
                    refreshFollowList(hwnd, state);
                    // 把刚加的那条选中并滚进视野：否则用户加完看不到反馈，
                    // 会以为按钮没生效（列表超过四行时新的那条正好在下面）。
                    HWND list = GetDlgItem(hwnd, kFollowListId);
                    if (list != nullptr && !state->followLaunch.empty()) {
                        const int last =
                            static_cast<int>(state->followLaunch.size()) - 1;
                        ListView_SetItemState(
                            list, last, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(list, last, FALSE);
                        SetFocus(list);
                    }
                }
                return 0;
            }
            if (controlId == kFollowRemoveId && notification == BN_CLICKED) {
                if (state != nullptr && removeFollowEntry(hwnd, state)) {
                    refreshFollowList(hwnd, state);
                }
                return 0;
            }

            // ---- 轮盘：打开扇区动作编辑器 ----
            //
            // 子对话框直接改 state->radial（设置窗口的草稿副本），返回 true
            // 说明用户点了确定，此时刷新概况行；点取消则草稿原样未动。
            if (controlId == kRadialEditButtonId &&
                notification == BN_CLICKED) {
                if (state != nullptr) {
                    if (showRadialEditorWindow(hwnd, &state->radial)) {
                        refreshRadialSummary(hwnd, state);
                    }
                }
                return 0;
            }

            // ---- 插件管理：安装（选择 plugin.json → 复制整个目录）----
            if (controlId == kPluginInstallId && notification == BN_CLICKED) {
                if (state != nullptr && state->plugins != nullptr &&
                    state->plugins->install) {
                    wchar_t file[MAX_PATH] = {};
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    // 过滤器里 \0 是分隔符，所以只能拼：描述 + \0 + 通配 + \0...
                    const std::wstring manifestFilter =
                        infrastructure::i18n::trw("filter.pluginManifest") +
                        L'\0' + L"plugin.json" + L'\0' +
                        infrastructure::i18n::trw("filter.allFiles") +
                        L'\0' + L"*.*" + L'\0';
                    ofn.lpstrFilter = manifestFilter.c_str();
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn) != FALSE) {
                        std::string error;
                        if (state->plugins->install(file, &error)) {
                            refreshPluginList(hwnd, state);
                        } else {
                            const std::wstring failureText = base::utf8ToWide(
                                error.empty()
                                    ? std::string(infrastructure::i18n::tr(
                                          "settings.plugins.installFailedShort"))
                                    : error);
                            MessageBoxW(hwnd, failureText.c_str(),
                                        T("settings.plugins.installFailed"),
                                        MB_OK | MB_ICONWARNING);
                        }
                    }
                }
                return 0;
            }

            // ---- 插件管理：添加磁贴 / 打开目录 / 删除 ----
            if ((controlId == kPluginAddTileId ||
                 controlId == kPluginOpenFolderId ||
                 controlId == kPluginRemoveId) &&
                notification == BN_CLICKED) {
                if (state == nullptr || state->plugins == nullptr) {
                    return 0;
                }
                const std::wstring id = selectedPluginId(hwnd, state);
                if (controlId == kPluginOpenFolderId) {
                    // 没选中任何插件也能用：打开插件根目录，方便用户
                    // 手工往里放文件夹。
                    if (state->plugins->openFolder) {
                        state->plugins->openFolder(base::wideToUtf8Lossy(id));
                    }
                    return 0;
                }
                if (id.empty()) {
                    MessageBoxW(hwnd, T("settings.plugins.selectFirst"),
                                T("settings.tab.plugins"), MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                if (controlId == kPluginAddTileId) {
                    if (state->plugins->addTile) {
                        state->plugins->addTile(base::wideToUtf8Lossy(id));
                        MessageBoxW(hwnd, T("settings.plugins.added"),
                                    T("settings.tab.plugins"), MB_OK | MB_ICONINFORMATION);
                    }
                    return 0;
                }

                const std::wstring question =
                    infrastructure::i18n::formatW(
                        "settings.plugins.removeConfirm", {id});
                if (MessageBoxW(hwnd, question.c_str(), T("settings.plugins.remove"),
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                    IDYES) {
                    return 0;
                }
                std::string error;
                if (state->plugins->remove &&
                    state->plugins->remove(base::wideToUtf8Lossy(id), &error)) {
                    refreshPluginList(hwnd, state);
                } else {
                    const std::wstring failureText = base::utf8ToWide(
                        error.empty()
                            ? std::string(infrastructure::i18n::tr(
                                  "settings.plugins.removeFailedShort"))
                            : error);
                    MessageBoxW(hwnd, failureText.c_str(), T("settings.plugins.removeFailed"),
                                MB_OK | MB_ICONWARNING);
                }
                return 0;
            }

            // ---- 皮肤：切换即预览 ----
            //
            // 只通知调用方「换成哪套了」，不落盘、不改 state->result ——
            // 真正的提交仍然只看「确定」那一刻选了谁。取消时由调用方
            // 重新套用配置里的皮肤，预览自动消失。
            if (controlId == kThemeComboId && notification == CBN_SELCHANGE) {
                if (state != nullptr && state->preview != nullptr &&
                    state->preview->themeChanged) {
                    const std::wstring name = ui::canonicalThemeName(
                        comboSelectedText(hwnd, kThemeComboId));
                    if (!name.empty()) {
                        state->preview->themeChanged(
                            base::wideToUtf8Lossy(name));
                    }
                }
                return 0;
            }

            // ---- 标签栏位置：选中即预览（纯几何平移，零风险）----
            //
            // radio 是互斥的，只有「被选中的那个」会走到这里，
            // 所以按 controlId 就能判定方向，不必再读选中态。
            if ((controlId == kTabTopRadioId ||
                 controlId == kTabBottomRadioId) &&
                notification == BN_CLICKED) {
                if (state != nullptr && state->preview != nullptr &&
                    state->preview->tabBarAtBottomChanged) {
                    state->preview->tabBarAtBottomChanged(
                        controlId == kTabBottomRadioId);
                }
                return 0;
            }

            // ---- 背景图：选择图片 ----
            if (controlId == kBackgroundBrowseId &&
                notification == BN_CLICKED) {
                if (state != nullptr) {
                    wchar_t file[MAX_PATH] = {};
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    // 过滤器里 NUL 是分隔符，只能运行时拼（文案来自语言文件）
                    const std::wstring imageFilter =
                        infrastructure::i18n::trw("filter.images") + L'\0' +
                        L"*.png;*.jpg;*.jpeg;*.bmp;*.gif" + L'\0' +
                        infrastructure::i18n::trw("filter.allFiles") + L'\0' +
                        L"*.*" + L'\0';
                    ofn.lpstrFilter = imageFilter.c_str();
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn) != FALSE) {
                        state->pickedBackgroundPath = file;
                        state->backgroundCleared = false;
                        // 立即把这张图贴到主窗口上——先看效果再决定是否确定
                        if (state->preview != nullptr &&
                            state->preview->backgroundChanged) {
                            state->preview->backgroundChanged(
                                state->pickedBackgroundPath);
                        }
                    }
                }
                return 0;
            }

            // ---- 背景图：清除 ----
            if (controlId == kBackgroundClearId &&
                notification == BN_CLICKED) {
                if (state != nullptr) {
                    state->backgroundCleared = true;
                    state->pickedBackgroundPath.clear();
                    // 主窗口同步撤掉背景图（仍只是预览，未落盘）
                    if (state->preview != nullptr &&
                        state->preview->backgroundChanged) {
                        state->preview->backgroundChanged(std::wstring());
                    }
                }
                return 0;
            }

            // ---- 磁贴背景色：选色 / 恢复跟随皮肤（都实时预览）----
            //
            // 与其它外观项一样只通告预览，不写 state->result：
            // 提交仍然只看「确定」那一刻的选择。
            // 色块即入口：STATIC 只有带 SS_NOTIFY 才会发 STN_CLICKED。
            // 双击会先给 STN_CLICKED 再给 STN_DBLCLK，只认前者，避免
            // 连开两个调色板。
            if (controlId == kDefaultTileColorSwatchId &&
                notification == STN_CLICKED) {
                if (state != nullptr) {
                    // 初始色：已选则用它；否则用皮肤磁贴底的近似值，
                    // 让调色板打开时勾在用户现在看到的那一格附近。
                    const std::uint32_t initial =
                        state->chosenDefaultTileColor.value_or(0xFF3A3A3EU);
                    // 内置色表：每点一格立刻刷新主窗口预览，弹板不关，
                    // 方便连续换色对比；最终值仍由「确定」提交。
                    showColorPalette(
                        hwnd, GetDlgItem(hwnd, kDefaultTileColorSwatchId),
                        initial,
                        [hwnd, state](const std::uint32_t color) {
                            state->chosenDefaultTileColor = color;
                            state->defaultTileColorTouched = true;
                            InvalidateRect(
                                GetDlgItem(hwnd, kDefaultTileColorSwatchId),
                                nullptr, TRUE);
                            if (state->preview != nullptr &&
                                state->preview->defaultTileColorChanged) {
                                state->preview->defaultTileColorChanged(color);
                            }
                        });
                }
                return 0;
            }
            if (controlId == kDefaultTileColorClearId &&
                notification == BN_CLICKED) {
                if (state != nullptr) {
                    state->chosenDefaultTileColor.reset();
                    state->defaultTileColorTouched = true;
                    InvalidateRect(
                        GetDlgItem(hwnd, kDefaultTileColorSwatchId),
                        nullptr, TRUE);
                    if (state->preview != nullptr &&
                        state->preview->defaultTileColorChanged) {
                        state->preview->defaultTileColorChanged(std::nullopt);
                    }
                }
                return 0;
            }

            if (controlId == kOkId) {
                if (state != nullptr) {
                    const int columns = readSpinnerValue(
                        hwnd, kPropColumnsEdit, state->initialColumns);
                    const int rows = readSpinnerValue(
                        hwnd, kPropRowsEdit, state->initialRows);

                    // 规范化回写，让用户看到被夹取后的真实值
                    writeSpinnerValue(hwnd, kPropColumnsEdit, columns);
                    writeSpinnerValue(hwnd, kPropRowsEdit, rows);

                    // 只记录「真的改了」的项，其余留空 —— 调用方据此
                    // 跳过无意义的落盘与布局重建。
                    if (columns != static_cast<int>(state->initialColumns)) {
                        state->result.gridColumns =
                            static_cast<std::uint16_t>(columns);
                    }
                    if (rows != static_cast<int>(state->initialRows)) {
                        state->result.gridRows =
                            static_cast<std::uint16_t>(rows);
                    }

                    // ---- 文字标签（三态单选）----
                    // 「始终不显示」= 总闸关；另外两项都显示，区别在常驻与否。
                    // 隐藏时不改 alwaysShowTileLabels：它此时无意义，保留原值
                    // 可以让用户切回「始终显示」/「移动时显示」时回到原来的
                    // 选择。
                    {
                        const bool labelsNever =
                            SendMessageW(GetDlgItem(hwnd, kTitleNeverRadioId),
                                         BM_GETCHECK, 0, 0) == BST_CHECKED;
                        const bool labelsAlways =
                            SendMessageW(GetDlgItem(hwnd, kTitleAlwaysRadioId),
                                         BM_GETCHECK, 0, 0) == BST_CHECKED;
                        const bool show = !labelsNever;
                        if (show != state->initialShowLabels) {
                            state->result.showTileLabels = show;
                        }
                        if (show && labelsAlways !=
                                        state->initialAlwaysShowTitle) {
                            state->result.alwaysShowTileLabels = labelsAlways;
                        }
                    }

                    // ---- 语言 ----
                    //
                    // 只在与初值不同时才回传：应用层据此判断要不要落盘 +
                    // 重新取词。值本身可以是 "auto"（跟随系统）。
                    {
                        const std::string language = selectedLanguageCode(hwnd);
                        if (language != state->initialLanguage) {
                            state->result.language = language;
                        }
                    }

                    // ---- 标签栏位置（单选）----
                    HWND tabBottom = GetDlgItem(hwnd, kTabBottomRadioId);
                    if (tabBottom != nullptr) {
                        const bool checked =
                            SendMessageW(tabBottom, BM_GETCHECK, 0, 0) ==
                            BST_CHECKED;
                        if (checked != state->initialTabBarAtBottom) {
                            state->result.tabBarAtBottom = checked;
                        }
                    }

                    // ---- 窗口出现位置（下拉索引 → 映射表）----
                    HWND placementCombo =
                        GetDlgItem(hwnd, kWindowPlacementComboId);
                    if (placementCombo != nullptr) {
                        const int selection = static_cast<int>(
                            SendMessageW(placementCombo, CB_GETCURSEL, 0, 0));
                        if (selection >= 0 &&
                            selection < kPlacementOptionCount) {
                            const auto placement =
                                kPlacementOptions[selection].value;
                            if (placement != state->initialWindowPlacement) {
                                state->result.windowPlacement = placement;
                            }
                        }
                    }

                    // ---- 皮肤 ----
                    const std::wstring themeName = ui::canonicalThemeName(
                        comboSelectedText(hwnd, kThemeComboId));
                    if (!themeName.empty() &&
                        themeName != state->initialThemeName) {
                        state->result.themeName =
                            base::wideToUtf8Lossy(themeName);
                    }

                    // ---- 背景图：清除优先于新选（都点了以清除为准）----
                    if (state->backgroundCleared) {
                        state->result.backgroundImage = std::string();
                    } else if (!state->pickedBackgroundPath.empty()) {
                        state->result.backgroundImage =
                            base::wideToUtf8Lossy(state->pickedBackgroundPath);
                    }

                    HWND startup = GetDlgItem(hwnd, kStartupCheckId);
                    if (startup != nullptr) {
                        const bool checked =
                            SendMessageW(startup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (checked != state->initialStartup) {
                            state->result.startWithWindows = checked;
                        }
                    }

                    // 跟随启动：整份交回（顺序也算改动 —— 用户可能就想要
                    // 这个先后顺序）。
                    if (!sameFollowLaunch(state->followLaunch,
                                          state->initialFollowLaunch)) {
                        state->result.followLaunch = state->followLaunch;
                    }

                    // ---- 唤醒快捷键（单选组，找出被选中的那个）----
                    {
                        struct PresetRadio {
                            int id;
                            core::HotkeyPreset preset;
                        };
                        const PresetRadio radios[] = {
                            {kHotkeyAltSpaceRadioId,
                             core::HotkeyPreset::AltSpace},
                            {kHotkeyCtrlSpaceRadioId,
                             core::HotkeyPreset::CtrlSpace},
                            {kHotkeyDoubleCtrlRadioId,
                             core::HotkeyPreset::DoubleCtrl},
                            {kHotkeyWinKeyRadioId, core::HotkeyPreset::WinKey},
                        };
                        for (const auto& radio : radios) {
                            HWND button = GetDlgItem(hwnd, radio.id);
                            if (button == nullptr) {
                                continue;
                            }
                            if (SendMessageW(button, BM_GETCHECK, 0, 0) !=
                                BST_CHECKED) {
                                continue;
                            }
                            if (radio.preset != state->initialHotkeyPreset) {
                                state->result.hotkeyPreset = radio.preset;
                            }
                            break;
                        }
                    }

                    // ---- 不透明度（只有真正拖动过才记录）----
                    //
                    // 磁贴背景的初始值是「皮肤实际的 alpha」而不是配置里的
                    // -1。用户没碰滑块就不该落盘 —— 否则「跟随皮肤」会被
                    // 写死成一个数字，之后换皮肤就不跟着变了。
                    if (state->windowOpacityTouched) {
                        HWND slider =
                            GetDlgItem(hwnd, kWindowOpacitySliderId);
                        if (slider != nullptr) {
                            const int percent = static_cast<int>(
                                SendMessageW(slider, TBM_GETPOS, 0, 0));
                            if (percent != state->initialWindowOpacity) {
                                state->result.windowOpacityPercent = percent;
                            }
                        }
                    }
                    if (state->tileOpacityTouched) {
                        HWND slider = GetDlgItem(hwnd, kTileOpacitySliderId);
                        if (slider != nullptr) {
                            const int percent = static_cast<int>(
                                SendMessageW(slider, TBM_GETPOS, 0, 0));
                            if (percent != state->initialTileOpacity) {
                                state->result.tileOpacityPercent = percent;
                            }
                        }
                    }
                    if (state->iconScaleTouched) {
                        HWND slider = GetDlgItem(hwnd, kIconScaleSliderId);
                        if (slider != nullptr) {
                            const int percent = static_cast<int>(
                                SendMessageW(slider, TBM_GETPOS, 0, 0));
                            if (percent != state->initialIconScale) {
                                state->result.iconScalePercent = percent;
                            }
                        }
                    }

                    // ---- 磁贴背景色 ----
                    //
                    // 必须用 touched 而不是单看值：内层空值（取消选色、
                    // 回到跟随皮肤）也是有效改动，与「没动过」不同。
                    if (state->defaultTileColorTouched &&
                        state->chosenDefaultTileColor !=
                            state->initialDefaultTileColor) {
                        state->result.defaultTileColor =
                            state->chosenDefaultTileColor;
                    }

                    // ---- 轮盘菜单 ----
                    //
                    // 触发方式在这里从控件读取；扇区动作与按应用规则早已在
                    // state->radial 里（由 radial_editor_dialog 改的就是这份
                    // 草稿）。整份交回，由应用层决定落盘与重装钩子。
                    {
                        HWND enabled = GetDlgItem(hwnd, kRadialEnabledCheckId);
                        if (enabled != nullptr) {
                            state->radial.trigger.enabled =
                                SendMessageW(enabled, BM_GETCHECK, 0, 0) ==
                                BST_CHECKED;
                        }
                        HWND buttonCombo =
                            GetDlgItem(hwnd, kRadialButtonComboId);
                        if (buttonCombo != nullptr) {
                            const int selection = static_cast<int>(
                                SendMessageW(buttonCombo, CB_GETCURSEL, 0, 0));
                            if (selection >= 0 &&
                                selection < kRadialButtonOptionCount) {
                                state->radial.trigger.button =
                                    kRadialButtonOptions[selection].value;
                            }
                        }
                        HWND modifierCombo =
                            GetDlgItem(hwnd, kRadialModifierComboId);
                        if (modifierCombo != nullptr) {
                            const int selection = static_cast<int>(
                                SendMessageW(modifierCombo, CB_GETCURSEL, 0, 0));
                            if (selection >= 0 &&
                                selection < kRadialModifierOptionCount) {
                                state->radial.trigger.modifier =
                                    kRadialModifierOptions[selection].value;
                            }
                        }
                        HWND threshold =
                            GetDlgItem(hwnd, kRadialThresholdSliderId);
                        if (threshold != nullptr) {
                            state->radial.trigger.activationThresholdPx =
                                static_cast<std::uint16_t>(
                                    SendMessageW(threshold, TBM_GETPOS, 0, 0));
                        }
                        HWND cancel = GetDlgItem(hwnd, kRadialCancelSliderId);
                        if (cancel != nullptr) {
                            state->radial.trigger.cancelRadiusPx =
                                static_cast<std::uint16_t>(
                                    SendMessageW(cancel, TBM_GETPOS, 0, 0));
                        }
                        // 抑制键：从下拉的 item data 重建列表。与触发修饰键
                        // 撞车的直接丢掉，保证交给 core 的配置必然合法（
                        // tooltip 里已经写明这条规则，用户不会莫名其妙）。
                        state->radial.trigger.suppressWhileHeld.clear();
                        HWND suppressCombo =
                            GetDlgItem(hwnd, kRadialSuppressComboId);
                        for (int i = 0; i < kRadialSuppressOptionCount; ++i) {
                            const RadialSuppressOption& option =
                                kRadialSuppressOptions[i];
                            if (!suppressChecked(suppressCombo, i)) {
                                continue;
                            }
                            if (core::modifierIsTriggerModifier(
                                    option.value,
                                    state->radial.trigger.modifier)) {
                                continue;
                            }
                            state->radial.trigger.suppressWhileHeld.push_back(
                                option.value);
                        }
                        state->result.radial = state->radial;
                    }

                    state->accepted = true;
                }
                DestroyWindow(hwnd);
                return 0;
            }

            if (controlId == kCancelId) {
                if (state != nullptr) {
                    state->accepted = false;
                }
                DestroyWindow(hwnd);
                return 0;
            }

            // 编辑框内容变化 → 刷新提示文案 + 实时预览网格。
            // 上下调节器会改写编辑框内容，因此它的事件也一并覆盖。
            if ((controlId == kColumnsEditId || controlId == kRowsEditId)) {
                if (notification == EN_CHANGE) {
                    refreshHint(hwnd, state);
                    if (state != nullptr && state->preview != nullptr &&
                        state->preview->gridChanged) {
                        // 读值会夹到合法范围（用户可能正在输「1」），
                        // 但**不回写控件** —— 中途改写会把用户输入吃掉。
                        const int columns = readSpinnerValue(
                            hwnd, kPropColumnsEdit, state->initialColumns);
                        const int rows = readSpinnerValue(
                            hwnd, kPropRowsEdit, state->initialRows);
                        state->preview->gridChanged(
                            static_cast<std::uint16_t>(columns),
                            static_cast<std::uint16_t>(rows));
                        // 主窗口尺寸变了，设置窗口重新躲到旁边
                        positionSettingsWindow(hwnd,
                            GetWindow(hwnd, GW_OWNER), false);
                    }
                }
                return 0;
            }

            // 文字标签三态单选：选中即预览。
            // 原来的「显示标签」复选框 + 「默认行为」二选一被合并成一行三项，
            // 因此这里要同时推出两个布尔量：
            //   show  = 不是「始终不显示」
            //   always = 是否选的是「始终显示」
            // 「移动时显示」时 show=true / always=false，正好等价于旧组合。
            if ((controlId == kTitleAlwaysRadioId ||
                 controlId == kTitleDynamicRadioId ||
                 controlId == kTitleNeverRadioId) &&
                notification == BN_CLICKED) {
                if (state != nullptr && state->preview != nullptr) {
                    const bool show = controlId != kTitleNeverRadioId;
                    const bool always = controlId == kTitleAlwaysRadioId;
                    if (state->preview->showTileLabelsChanged) {
                        state->preview->showTileLabelsChanged(show);
                    }
                    if (show && state->preview->alwaysShowTitleChanged) {
                        state->preview->alwaysShowTitleChanged(always);
                    }
                }
                return 0;
            }

            // 上下箭头按下时，编辑框的 EN_CHANGE 未必立即到达，
            // 这里补一次刷新，保证提示跟着数字走。
            if (controlId == kColumnsSpinId || controlId == kRowsSpinId) {
                refreshHint(hwnd, state);
                return 0;
            }
            break;
        }

        case WM_HSCROLL: {
            // 两个不透明度滑块走 WM_HSCROLL（trackbar 的约定），
            // 与 WM_COMMAND 的复选框/单选是两条通知路径。
            const HWND control = reinterpret_cast<HWND>(lParam);
            if (control == nullptr) {
                break;
            }
            const int controlId = GetDlgCtrlID(control);

            // 轮盘页的两个像素滑块：拖动时只刷新数值文字，
            // 真正的模型更新在「确定」时统一从控件读一次（单一来源，
            // 不会出现「拖过但没读」的中间态）。
            if (controlId == kRadialThresholdSliderId ||
                controlId == kRadialCancelSliderId) {
                const int value = static_cast<int>(
                    SendMessageW(control, TBM_GETPOS, 0, 0));
                const int valueId = controlId == kRadialThresholdSliderId
                                        ? kRadialThresholdValueId
                                        : kRadialCancelValueId;
                HWND label = GetDlgItem(hwnd, valueId);
                if (label != nullptr) {
                    const std::wstring text = std::to_wstring(value) + infrastructure::i18n::trw("unit.pixels");
                    SetWindowTextW(label, text.c_str());
                }
                return 0;
            }

            if (controlId != kWindowOpacitySliderId &&
                controlId != kTileOpacitySliderId &&
                controlId != kIconScaleSliderId) {
                break;
            }

            const int percent =
                static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            const int valueId = controlId == kWindowOpacitySliderId
                ? kWindowOpacityValueId
                : controlId == kTileOpacitySliderId
                      ? kTileOpacityValueId
                      : kIconScaleValueId;
            HWND value = GetDlgItem(hwnd, valueId);
            if (value != nullptr) {
                const std::wstring text = std::to_wstring(percent) + L"%";
                SetWindowTextW(value, text.c_str());
            }

            if (state != nullptr) {
                if (controlId == kWindowOpacitySliderId) {
                    state->windowOpacityTouched = true;
                    if (state->preview != nullptr &&
                        state->preview->windowOpacityChanged) {
                        state->preview->windowOpacityChanged(percent);
                    }
                } else if (controlId == kTileOpacitySliderId) {
                    state->tileOpacityTouched = true;
                    if (state->preview != nullptr &&
                        state->preview->tileOpacityChanged) {
                        state->preview->tileOpacityChanged(percent);
                    }
                } else {
                    state->iconScaleTouched = true;
                    if (state->preview != nullptr &&
                        state->preview->iconScaleChanged) {
                        state->preview->iconScaleChanged(percent);
                    }
                }
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // 所有 STATIC 都在白色卡片内，统一白底 + 正文色；
            // 提示行用次级灰拉开层次。返回画刷必须与卡片一致，
            // 否则文字后面会出现一块与卡片不同的色块。
            HDC dc = reinterpret_cast<HDC>(wParam);
            const int controlId =
                GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            const bool isSection = controlId >= kSectionHeaderIdBase &&
                                   controlId < kSectionHeaderIdEnd;
            const bool isHint =
                controlId == kHintId || controlId == kPluginHintId;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, (isHint && !isSection)
                                 ? dialog_style::secondaryText()
                                 : dialog_style::text());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_CTLCOLORBTN: {
            // 单选框 / 复选框不在卡片里时会被系统用 COLOR_BTNFACE 填底，
            // 在白色卡片上就是一块灰方框；这里统一改成白底。
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dialog_style::text());
            SetBkColor(dc, dialog_style::card());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_CLOSE:
            // 关闭窗口等同于取消，避免「关掉窗口但改动被默默套用」
            if (state != nullptr) {
                state->accepted = false;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            HFONT font = static_cast<HFONT>(RemovePropW(hwnd, kPropFont));
            if (font != nullptr) {
                DeleteObject(font);
            }
            HFONT sectionFont =
                static_cast<HFONT>(RemovePropW(hwnd, kPropSectionFont));
            if (sectionFont != nullptr) {
                DeleteObject(sectionFont);
            }
            HFONT tabFont =
                static_cast<HFONT>(RemovePropW(hwnd, kPropTabFont));
            if (tabFont != nullptr) {
                DeleteObject(tabFont);
            }
            RemovePropW(hwnd, kPropColumnsEdit);
            RemovePropW(hwnd, kPropRowsEdit);
            RemovePropW(hwnd, kPropHint);
            RemovePropW(hwnd, kPropPluginList);

            // 跟随启动的图标表：列表控件只是「用了它」，销毁列表不会回收
            // 图标句柄，必须在这里显式清掉，否则每开一次设置漏一批 HICON。
            if (state != nullptr && state->followIcons != nullptr) {
                ImageList_Destroy(state->followIcons);
                state->followIcons = nullptr;
            }

            // 通知模态循环「本对话框已关闭」。
            //
            // 为什么不用 PostQuitMessage：
            //   它投递的是**线程级** WM_QUIT，而且是粘性的 ——
            //   除了结束本对话框的嵌套循环，还会让主消息循环在下次
            //   GetMessage 时立刻拿到 WM_QUIT 并退出，表现为
            //   「点设置窗口的确定 → 整个程序退出」。
            //   改用自定义消息，只结束本对话框的循环，主循环不受影响。
            PostMessageW(hwnd, kDialogCloseMessage, 0, 0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// 注册窗口类（进程内只注册一次）
void ensureSettingsDialogClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &settingsDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kSettingsDialogClass;

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

// 把设置窗口摆到主窗口外侧：右 → 左 → 下 → 上。
//
// coverCenterFallback = true（首次打开）：四面都放不下时居中覆盖主窗口，
//   这是窄屏 + 大网格的极端情形，宁可靠中间也别跑到屏幕角落。
// coverCenterFallback = false（网格预览中重新摆位）：此时主窗口正在变大/
//   变小，若居中就会被主窗口整个盖住 —— 预览白做。改为退到工作区右下角，
//   保证设置窗口本身始终可见。
//
// 用当前窗口矩形（含标题栏边框）算尺寸，不按设计稿推算 —— DPI 与
// 边框厚度都自动正确。
void positionSettingsWindow(HWND dialog, HWND owner,
                            const bool coverCenterFallback) {
    if (dialog == nullptr) {
        return;
    }

    RECT windowRect{};
    if (GetWindowRect(dialog, &windowRect) == FALSE) {
        return;
    }
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int gap = scaleFor(owner, 12);
    int x = 0;
    int y = 0;
    bool placed = false;

    RECT anchor{};
    if (owner != nullptr && IsWindowVisible(owner) != FALSE &&
        GetWindowRect(owner, &anchor) != FALSE) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor =
            MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
            const RECT work = monitorInfo.rcWork;
            const int anchorWidth = anchor.right - anchor.left;
            const int anchorHeight = anchor.bottom - anchor.top;

            if (anchor.right + gap + width <= work.right) {
                x = anchor.right + gap;
                y = anchor.top + (anchorHeight - height) / 2;
                placed = true;
            } else if (anchor.left - gap - width >= work.left) {
                x = anchor.left - gap - width;
                y = anchor.top + (anchorHeight - height) / 2;
                placed = true;
            } else if (anchor.bottom + gap + height <= work.bottom) {
                x = anchor.left + (anchorWidth - width) / 2;
                y = anchor.bottom + gap;
                placed = true;
            } else if (anchor.top - gap - height >= work.top) {
                x = anchor.left + (anchorWidth - width) / 2;
                y = anchor.top - gap - height;
                placed = true;
            }

            if (!placed && !coverCenterFallback) {
                // 网格预览中：躲到工作区右下角，别压在主窗口中间
                x = work.right - width - gap;
                y = work.bottom - height - gap;
                if (x < work.left) x = work.left;
                if (y < work.top) y = work.top;
                placed = true;
            }

            if (placed) {
                // 夹进工作区：主窗口贴着屏幕边缘时，上面算出的位置
                // 可能有一半在屏幕外（尤其垂直居中那一路）。
                if (x < work.left) x = work.left;
                if (y < work.top) y = work.top;
                if (x + width > work.right) x = work.right - width;
                if (y + height > work.bottom) y = work.bottom - height;
            }
        }
    }

    if (!placed) {
        // 回落：居中于 owner；owner 为空时居中于主显示器
        if (owner != nullptr && GetWindowRect(owner, &anchor) != FALSE) {
            x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
            y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
        } else {
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
            y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        }
    }

    SetWindowPos(dialog, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

// ---------------------------------------------------------------- 对外入口

bool showSettingsWindow(HWND owner,
                        const std::uint16_t currentColumns,
                        const std::uint16_t currentRows,
                        const bool currentShowLabels,
                        const bool currentAlwaysShowTitle,
                        bool currentTabBarAtBottom,
                        core::WindowPlacement currentWindowPlacement,
                        const std::string& currentThemeName,
                        const std::string& currentLanguage,
                        const bool currentStartup,
                        const std::vector<core::FollowLaunchEntry>&
                            currentFollowLaunch,
                        const core::HotkeyPreset currentHotkeyPreset,
                        const int currentWindowOpacity,
                        const int currentTileOpacity,
                        const int currentIconScale,
                        const std::optional<std::uint32_t>&
                            currentDefaultTileColor,
                        const core::RadialConfig& currentRadial,
                        const std::uint16_t columnsInUse,
                        const std::uint16_t rowsInUse,
                        SettingsResult* out,
                        const SettingsPreviewHooks* preview,
                        const SettingsPluginHooks* plugins,
                        const SettingsRadialPluginHooks* radialPlugins) {
    if (out == nullptr) {
        return false;
    }

    // 上下调节器与插件列表都属于 comctl32 的「Common Controls」。
    // 主程序启动时未必初始化过，这里补一次（重复调用无害）。
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(INITCOMMONCONTROLSEX);
    controls.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES |
                     ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    ensureSettingsDialogClass(instance);

    SettingsDialogState state;
    state.initialColumns = currentColumns;
    state.initialRows = currentRows;
    state.initialShowLabels = currentShowLabels;
    state.initialAlwaysShowTitle = currentAlwaysShowTitle;
    state.initialTabBarAtBottom = currentTabBarAtBottom;
    state.initialWindowPlacement = currentWindowPlacement;
    state.initialThemeName = base::utf8ToWide(currentThemeName);
    state.initialLanguage.assign(currentLanguage);
    state.initialStartup = currentStartup;
    state.initialFollowLaunch = currentFollowLaunch;
    state.followLaunch = currentFollowLaunch;
    state.initialHotkeyPreset = currentHotkeyPreset;
    state.initialWindowOpacity = currentWindowOpacity;
    state.initialTileOpacity = currentTileOpacity;
    state.initialIconScale = currentIconScale;
    state.initialDefaultTileColor = currentDefaultTileColor;
    state.chosenDefaultTileColor = currentDefaultTileColor;
    state.radial = currentRadial;
    state.columnsInUse = columnsInUse;
    state.rowsInUse = rowsInUse;
    state.preview = preview;
    state.plugins = plugins;
    state.radialPlugins = radialPlugins;

    // ---- 尺寸：客户区按设计稿，换算成含标题栏边框的窗口尺寸 ----
    RECT desired{0, 0, scaleFor(owner, kDialogWidth),
                 scaleFor(owner, kDialogHeight)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    HWND dialog = CreateWindowExW(
        exStyle,
        kSettingsDialogClass,
        T("settings.windowTitle"),
        style,
        0, 0,
        desired.right - desired.left,
        desired.bottom - desired.top,
        owner,
        nullptr,
        instance,
        &state);

    if (dialog == nullptr) {
        return false;
    }

    // ---- 定位：尽量不压住主窗口（右 → 左 → 下 → 上）----
    //
    // 设置窗口打开期间主窗口保持可见（应用层加了模态保护），各项设置
    // 都要实时看到效果 —— 所以优先排在主窗口外侧。四面都放不下才
    // 回落到居中覆盖（窄屏 + 大网格的极端情形）。
    positionSettingsWindow(dialog, owner, true);

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环。
    //
    // 与重命名对话框同一策略：用 GetMessage（不是 PeekMessage 忙等）；
    // 属于对话框的消息走 IsDialogMessage（Tab 切换 / 回车触发默认按钮），
    // 其余消息照常派发，保证托盘与热键在对话框打开时仍然工作。
    //
    // 退出条件用 IsWindow(dialog)：对话框销毁后这个条件为假，循环自然结束。
    // 不要依赖 GetMessage 返回 0 —— 那需要 WM_QUIT，而 WM_QUIT 会连带
    // 杀掉主消息循环（问题3的根因）。kDialogCloseMessage 只是把循环唤醒，
    // 真正的退出由 IsWindow 判定。
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kDialogCloseMessage) {
            continue;  // 唤醒用，不需要派发
        }
        if (message.hwnd == dialog || IsChild(dialog, message.hwnd)) {
            if (!IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (!state.accepted) {
        return false;
    }
    *out = state.result;
    return true;
}

}  // namespace ui
}  // namespace ktile
