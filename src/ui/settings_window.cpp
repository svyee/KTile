#include "ui/settings_window.h"

#include "base/text_convert.h"
#include "core/data_model.h"
#include "infrastructure/i18n.h"
#include "platform/win32/global_hotkey.h"
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

// --- 取词
// T() 是 i18n::trw 的短名（按当前语言查表，缺词回退英语）。返回的指针指向
// 语言字典的稳定存储，下次切语言前一直有效，可直接塞进 LPARAM / SetWindowTextW。
const wchar_t* T(const char* key) {
    return infrastructure::i18n::trw(key).c_str();
}

// --- 布局常量
// 全部按 96 DPI 逻辑像素书写，运行时统一用 scaleFor() 缩放。

// 窗口尺寸：左侧竖向标签栏 + 右侧单列表单，150% 缩放下不顶到工作区顶部。
constexpr int kDialogWidth = 700;
constexpr int kDialogHeight = 470;

constexpr int kMargin = 16;          // 窗口四周边距
constexpr int kContentTop = 14;      // 内容区起始 y

// ---- 左侧标签栏 ----
constexpr int kSidebarLeft = 16;
constexpr int kSidebarTop = 16;
constexpr int kSidebarWidth = 110;
constexpr int kSidebarTabHeight = 34;
constexpr int kSidebarTabGap = 4;

// --- 右侧内容区：单列表单
constexpr int kContentLeft = kSidebarLeft + kSidebarWidth + 16;        // 142
constexpr int kContentRight = kDialogWidth - kMargin;                  // 684
constexpr int kContentWidth = kContentRight - kContentLeft;            // 542
constexpr int kColumnWidth = 500;    // 表单内容宽度（卡片另加左右内边距）
// 单列布局下所有控件都以 kContentLeft 为基准
constexpr int kColumnRightX = kContentLeft;

constexpr int kRowGap = 12;          // 行间垂直间距
constexpr int kFormLabelMinWidth = 76;  // 标签列最小宽度（按语言实测，可能更宽）
constexpr int kComboWidth = 260;     // 下拉框宽度（单列下不铺满，视觉更稳）
// 内容卡片：白色圆角卡片包住控件，只画背景不改控件坐标。每页高度按内容量
// 定，白卡下沿的灰底即页面分界的视觉信号。
constexpr int kCardInset = 10;       // 卡片相对内容边界的左右外扩
constexpr int kCardTop = 8;
constexpr int kCardHeightAppearance = 410;  // 背景+磁贴+图标与标签+布局
// 语言 + 窗口 + 启动，其中「启动」含跟随启动列表（88px 的列表 + 标签行）。
constexpr int kCardHeightBehavior = 400;
constexpr int kCardHeightPlugins = 366;     // 说明+列表+按钮+脚注
// 轮盘插件页：脚注只有三行，按四行给高会在白卡下沿留出空带（实测可见）。
constexpr int kCardHeightRadialPlugins = 350;

// 轮盘页：说明文字进 tooltip 后只剩三行设置 + 扇区入口，白卡留空白会误导。
constexpr int kCardHeightRadial = 262;
// 附加小工具页：一节标题 + 一行快捷键 + 两行说明。
constexpr int kCardHeightTools = 132;
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

// 窗口位置下拉的展示顺序，与 core::WindowPlacement 枚举值刻意不对应：
// 枚举要保已存配置不错位（Center 只能追加在末尾），索引只对下拉负责。
// 文案只写 key，语言文件是唯一文案来源，取词在创建控件时做。
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

// 轮盘触发键 / 修饰键的下拉映射，键名与 data_model 一致，按常用程度排序。
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

// 抑制键：按住时轮盘不弹、鼠标事件放行。用 HotkeyModifier 是因为要覆盖
// Win，触发修饰键里没有它。勾选状态存组合框 item data（见 suppressChecked），
// 保存时与这一页其它控件同一种取数方式。
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
    // 只写「无」：加「独占该键」会把下拉撑宽，解释放在 tooltip。
    {"common.none", core::RadialTriggerModifier::None},
    {"key.ctrl", core::RadialTriggerModifier::Ctrl},
    {"key.shift", core::RadialTriggerModifier::Shift},
    {"key.alt", core::RadialTriggerModifier::Alt},
};

constexpr int kRadialModifierOptionCount = static_cast<int>(
    sizeof(kRadialModifierOptions) / sizeof(kRadialModifierOptions[0]));

// 触发方式第一行的三段横排（触发键 / 修饰键 / 抑制键），三者描述同一件事。
// 下拉宽度按当前语言选项实测：中文「右键拖动」52px，英文 "Middle-drag" 要
// 100 出头，写死会裁字；抑制键也不吃行尾剩余宽度，否则整行像被横向拉开。
constexpr int kRadialLabelWidthTight = 44;   // 标签列的最小宽度
constexpr int kRadialComboMinWidth = 84;     // 下拉框最小宽度
constexpr int kComboExtraWidth = 30;         // 下拉箭头 + 左右内边距

// --- 控件 ID

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
// 附加小工具页：快捷键录制按钮（点击后按键，Backspace 清除即关闭功能）
constexpr int kTopmostRecorderId = 1216;
constexpr int kHintId = 1120;
constexpr int kOkId = 1130;
constexpr int kCancelId = 1131;

// --- 分页与插件管理页
// 分页用自绘按钮竖排在左侧，不用 SysTabControl32：后者画带边框的页体，
// 与本对话框绝对坐标平铺的页面冲突；按钮分页只做显隐，零几何改动。
constexpr int kAppearanceTabId = 1135;
constexpr int kPluginsTabId = 1136;
constexpr int kBehaviorTabId = 1137;
constexpr int kToolsTabId = 1215;  // 附加小工具（窗口置顶等零散功能）

constexpr int kPluginListId = 1138;
constexpr int kPluginInstallId = 1170;
constexpr int kPluginAddTileId = 1171;
constexpr int kPluginOpenFolderId = 1172;
constexpr int kPluginRemoveId = 1173;
constexpr int kPluginHintId = 1174;

// --- 轮盘菜单页：只管「什么时候呼出」。扇区动作与按应用匹配在
// radial_editor 对话框里编辑（列表 + 表单，塞不进单列表单）。
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
// 抑制键是可勾选的下拉（Win32 无原生多选下拉）：自绘汇总与勾选方格，
// 子类化列表拦单击，否则点一行就收起，勾多项要反复展开。
constexpr int kRadialSuppressComboId = 1191;
// 抑制键标签带 ID：选修饰键时整组连标签一起变灰。
constexpr int kRadialSuppressLabelId = 1190;

// --- 轮盘插件页。ID 只能落在 1196~1200：1201 起是分组标题的连续区间
// （kSectionHeaderIdBase），插进去会破坏「灰色标题」判定。
constexpr int kRadialPluginsTabId = 1196;
constexpr int kRadialPluginListId = 1197;
constexpr int kRadialPluginOpenJsonId = 1198;
constexpr int kRadialPluginOpenFolderId = 1199;
constexpr int kRadialPluginRescanId = 1200;

// 滑块范围：阈值太小被手抖误触，太大拖半天不出轮盘；取消半径 150~400。
// 下限 150：花瓣外缘在 142 DIP，更小的值会被夹回，滑杆停在低处给用户
// 「拖了没反应」的错觉。
constexpr int kRadialThresholdMin = 4;
constexpr int kRadialThresholdMax = 40;
constexpr int kRadialCancelMin = 150;
constexpr int kRadialCancelMax = 400;

// 插件页：说明 + 列表 + 操作按钮 + 脚注（右侧内容区内自上而下排布）
constexpr int kPluginIntroTop = kContentTop;
constexpr int kPluginIntroHeight = 18;
constexpr int kPluginListTop = kPluginIntroTop + kPluginIntroHeight + 6;
constexpr int kPluginListHeight = 196;
constexpr int kPluginButtonTop = kPluginListTop + kPluginListHeight + 8;
constexpr int kPluginHintTop = kPluginButtonTop + kButtonHeight + 8;
// 脚注第三行会折成两行，高度要给够，否则 STATIC 白底会从卡片下沿冒出。
constexpr int kPluginHintHeight = 88;
constexpr int kPluginButtonWidth = 96;

// 分组标题用连续 ID 区间，WM_CTLCOLORSTATIC 里一次判定「画成灰色」。
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
constexpr int kSectionToolsId = 1210;          // 附加小工具：窗口置顶
constexpr int kSectionHeaderIdEnd = 1211;
// 语言下拉：1211 在分组标题区间之外，不会被 WM_CTLCOLORSTATIC 画灰。
constexpr int kLanguageComboId = 1211;

// --- 跟随启动
// 「图标 + 名称 + 路径」的列表加两个按钮：要显示图标让用户认出程序，
// 复选框做不出这一列。
constexpr int kFollowListId = 1212;
constexpr int kFollowAddId = 1213;
constexpr int kFollowRemoveId = 1214;
constexpr int kFollowIconSize = 18;  // 列表图标尺寸（逻辑像素）
constexpr int kFollowListHeight = 88;

// 控件句柄挂到窗口属性上，查找比到处 GetDlgItem 直白。
constexpr const wchar_t* kPropColumnsEdit = L"KTileSetColumnsEdit";
constexpr const wchar_t* kPropRowsEdit = L"KTileSetRowsEdit";
constexpr const wchar_t* kPropHint = L"KTileSetHint";
constexpr const wchar_t* kPropFont = L"KTileSetFont";
// 分页标签选中态的加粗字体（由 kPropFont 派生）。
constexpr const wchar_t* kPropTabFont = L"KTileSetTabFont";
constexpr const wchar_t* kPropSectionFont = L"KTileSetSectionFont";
constexpr const wchar_t* kPropPluginList = L"KTileSetPluginList";
constexpr const wchar_t* kPropRadialPluginList = L"KTileSetRadialPluginList";
// 悬停提示窗口，整个设置窗口共用一个
constexpr const wchar_t* kPropTooltip = L"KTileSetTooltip";

// --- 状态

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
    // 置顶快捷键：initial 用于判「改没改」，draft 是录制控件的当前值。
    core::HotkeyConfig initialTopmostHotkey;
    core::HotkeyConfig topmostHotkey;
    // 录制中：键盘输入归录制控件所有，不参与对话框导航
    bool topmostCapturing = false;
    // 录制刚结束：吞掉配对的 keyup。按钮在 Space 抬起时会发 BN_CLICKED，
    // 不吞等于刚录好 Space 就重新进入录制。
    bool swallowRecorderKeyUp = false;

    // 不透明度显示当前生效的百分比。touched 区分「拖动过」与「没动过」，
    // 否则点确定会把磁贴的「跟随皮肤」写死成数字。
    int initialWindowOpacity = 100;
    int initialTileOpacity = 24;
    bool windowOpacityTouched = false;
    bool tileOpacityTouched = false;
    int initialIconScale = 90;
    bool iconScaleTouched = false;

    // 全局默认磁贴底色：初始值 / 当前选择 / 是否动过。用 touched 而非比值，
    // 「取消选色」这种回到空值的变化也是有效改动。
    std::optional<std::uint32_t> initialDefaultTileColor;
    std::optional<std::uint32_t> chosenDefaultTileColor;
    bool defaultTileColorTouched = false;

    // 界面语言（配置里的原值："auto" 或具体语言代码）
    std::string initialLanguage;

    // 跟随启动：初值与草稿。「添加 / 移除」只改草稿，确定时才交回应用层。
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

    // 轮盘菜单配置的草稿。radial_editor 子对话框直接改这份副本，确定时才
    // 交回应用层，取消则整份丢弃，无需回滚。
    core::RadialConfig radial;

    // 分页：0 = 外观，1 = 行为，2 = 插件管理，3 = 轮盘，4 = 轮盘插件，
    // 5 = 附加小工具。各页控件已按绝对坐标摆好，切页只做显隐。
    int activePage = 0;
    std::vector<HWND> appearanceControls;
    std::vector<HWND> behaviorControls;
    std::vector<HWND> pluginControls;
    std::vector<HWND> radialControls;
    std::vector<HWND> radialPluginControls;
    std::vector<HWND> toolsControls;
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

// 对话框关闭通知，仅用于结束本对话框的模态循环。不用 WM_QUIT：线程级且
// 粘性，会连带终止主消息循环（见 WM_DESTROY 处说明）。
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

    // 用 wcstol 而不是 _wtoi：能区分「真的是 0」与「压根不是数字」。
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

// 更新提示文案：新尺寸比布局占用小时警告「会重排」。缩小网格不删磁贴，
// 超出的磁贴会被布局引擎重新安置，所以文案说的是重排不是丢失。
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

// --- 语言下拉

// 第 0 项固定是「跟随系统」，并在后面补上实际落到的语言
// （"跟随系统 · 简体中文"），否则系统语言与界面语言不一致时，
// 用户会怀疑设置没生效。
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
    // 配置里的代码已不存在（语言文件被删）：停在「跟随系统」上。
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

// --- 分页与插件列表

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

// 重新填充插件列表。每次切页 / 安装删除后都重问应用层：插件真相在磁盘上，
// 缓存会过期。
void refreshPluginList(HWND dialog, SettingsDialogState* state) {
    HWND list = static_cast<HWND>(GetPropW(dialog, kPropPluginList));
    if (list == nullptr || state == nullptr) {
        return;
    }

    // 记住选中项，刷新后停在原插件上
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

    // 没有选中项时灰掉「添加磁贴 / 删除」，灰掉比弹提示直接；
    // 「打开目录」始终可用（没选中时打开插件根目录）。
    const bool hasSelection = !selectedPluginId(dialog, state).empty();
    const int buttonIds[] = {kPluginAddTileId, kPluginRemoveId};
    for (const int id : buttonIds) {
        HWND button = GetDlgItem(dialog, id);
        if (button != nullptr) {
            EnableWindow(button, hasSelection ? TRUE : FALSE);
        }
    }
}

// 轮盘页的概况行：已配置扇区数 + 黑名单条数。扇区动作在子对话框里编辑，
// 这一行让用户确认改动还在。
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
    // 用占位符而不是自己拼句子：各语言语序不同，拼接会写死。
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

// --- 悬停提示

// 控件说明挂 tooltip 而不是成段灰字。TTF_SUBCLASS 让 tooltip 自己消化鼠标
// 消息，不用给每个控件写 TrackMouseEvent；comctl32 v5 自带，零新依赖。
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
    // 不给上限的话长解释会横贯屏幕
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
    // 进程没有 comctl32 v6 manifest，实际加载 v5.82，其 TOOLINFO 到 lpszText
    // 为止；按 sizeof 传会让 TTM_ADDTOOL 静默失败，表现为悬停死活不弹。
    // V1 尺寸在 v6 下同样被接受，两个版本都安全。
    info.cbSize = TTTOOLINFOW_V1_SIZE;
    // TTF_IDISHWND：uId 是控件句柄本身，一个控件一条 tip，不用编 ID
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = owner;
    info.uId = reinterpret_cast<UINT_PTR>(tool);
    info.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

// --- 抑制键下拉
// Win32 没有多选下拉，用 CBS_DROPDOWNLIST + CBS_OWNERDRAWFIXED 实现：
// 勾选状态存 item data，字段（WM_DRAWITEM 的 itemID == -1）画汇总文字，
// 列表子类化拦单击只切换勾选不收起。实测（build/probe_combo.cpp）：
// CB_SETCURSEL(-1) 时字段仍收到 itemID == -1 的 WM_DRAWITEM，不需要假的
// 摘要项；GetComboBoxInfo 未展开前就能拿到 hwndList，创建完即可子类化。

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

// 收起状态显示的文字：勾了几个列几个，一个没勾写「无」。
// 用顿号：任意一个按住都算，不是「一起按住」。
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

// 勾选方格用 DrawFrameControl 画，同时负责禁用态灰化。
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

    // 勾选方格按 DPI 定尺寸，不顶满行高（行高由字体决定，撑满会贴边）
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

// 列表的子类过程：单击切换勾选且不收起列表（原生行为点一行就收起选中）。
// 按下/抬起成对吞掉；点列表外仍按原生逻辑收起，那条路径不经过这里。
LRESULT CALLBACK suppressListProc(HWND list, UINT message, WPARAM wParam,
                                  LPARAM lParam, UINT_PTR /*subclassId*/,
                                  DWORD_PTR refData) {
    // 组合框句柄走 refData 传进来，不能用 GetParent(list) 反推：ComboLBox 的
    // 父窗口是内部窗口而非组合框，拿错句柄会静默失败（列表不收起但勾选没变）。
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
    // 没有「当前项」概念，收起字段是自绘汇总文字，见 paintSuppressField
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);

    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(combo, &info) != FALSE && info.hwndList != nullptr) {
        SetWindowSubclass(info.hwndList, suppressListProc, 1,
                          reinterpret_cast<DWORD_PTR>(combo));
    }
}

// --- 轮盘触发

// 修饰键非「无」时抑制键整组失效：选了修饰键，「不按修饰键」本身就是不
// 触发，抑制键再参与判断等于让同一个键既必须按又必须不按。
// 灰显而不清空勾选，切回「无」时原选择还在，不用重勾。
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
    // 自绘字段不随 WM_ENABLE 重画，补一次重绘让文字真的变灰。
    HWND combo = GetDlgItem(dialog, kRadialSuppressComboId);
    if (combo != nullptr) {
        InvalidateRect(combo, nullptr, TRUE);
    }
}

// --- 轮盘插件列表

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

// 重新填充轮盘插件列表。与磁贴插件页同策略：真相在磁盘上，每次切页 /
// 重新扫描后都重新问应用层要。
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
        // 只写「动作数 / 扇区数」：列头已有「扇区」二字，两数不等基本是配错。
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

    // 没有选中项时灰掉「打开 JSON / 打开目录」，重新扫描永远可用。
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

// --- 置顶快捷键录制
//
// 组合键自由录制，不比预设多的界面元素：点击按钮进入录制，之后键盘输入
// 全归它（WM_GETDLGCODE 挡住对话框的 Tab/Enter/Esc 导航）。按下非修饰键
// 即提交；Backspace/Delete 清空并关闭功能；Esc 放弃本次录制。

std::wstring hotkeyModifiersPrefix(
    const std::vector<core::HotkeyModifier>& modifiers) {
    std::wstring text;
    for (const auto modifier : modifiers) {
        switch (modifier) {
            case core::HotkeyModifier::Control: text += L"Ctrl+";  break;
            case core::HotkeyModifier::Shift:   text += L"Shift+"; break;
            case core::HotkeyModifier::Alt:     text += L"Alt+";   break;
            case core::HotkeyModifier::Win:     text += L"Win+";   break;
        }
    }
    return text;
}

bool isModifierVirtualKey(const UINT key) {
    return key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
           key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LMENU || key == VK_RMENU ||
           key == VK_LWIN || key == VK_RWIN;
}

// 按下的修饰键（固定 Ctrl → Shift → Alt → Win 顺序，显示才稳定）
std::vector<core::HotkeyModifier> pressedModifiers() {
    std::vector<core::HotkeyModifier> modifiers;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers.push_back(core::HotkeyModifier::Control);
    }
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers.push_back(core::HotkeyModifier::Shift);
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers.push_back(core::HotkeyModifier::Alt);
    }
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers.push_back(core::HotkeyModifier::Win);
    }
    return modifiers;
}

std::wstring topmostHotkeyDisplayText(const core::HotkeyConfig& config) {
    if (!config.enabled) {
        return std::wstring(T("common.none"));
    }
    return platform::win32::formatHotkey(config);
}

// 相等判定用在这里只为「改没改」，不追求 core 层的通用语义。
bool sameHotkeyConfig(const core::HotkeyConfig& left,
                      const core::HotkeyConfig& right) {
    return left.enabled == right.enabled && left.key == right.key &&
           left.modifiers == right.modifiers;
}

void refreshTopmostRecorder(HWND dialog, const SettingsDialogState* state) {
    HWND recorder = GetDlgItem(dialog, kTopmostRecorderId);
    if (recorder == nullptr || state == nullptr) {
        return;
    }
    SetWindowTextW(recorder,
                   topmostHotkeyDisplayText(state->topmostHotkey).c_str());
}

LRESULT CALLBACK topmostRecorderProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
    auto* state = reinterpret_cast<SettingsDialogState*>(refData);
    if (state == nullptr) {
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    if (!state->topmostCapturing) {
        if ((message == WM_KEYUP || message == WM_SYSKEYUP) &&
            state->swallowRecorderKeyUp) {
            state->swallowRecorderKeyUp = false;
            return 0;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    switch (message) {
        case WM_GETDLGCODE:
            return DefSubclassProc(hwnd, message, wParam, lParam) |
                   DLGC_WANTALLKEYS;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT key = static_cast<UINT>(wParam);
            if (key == VK_ESCAPE) {
                state->topmostCapturing = false;
                state->swallowRecorderKeyUp = true;
                refreshTopmostRecorder(GetParent(hwnd), state);
                return 0;
            }
            if (key == VK_BACK || key == VK_DELETE) {
                state->topmostHotkey = core::defaultTopmostHotkey();
                state->topmostHotkey.enabled = false;
                state->topmostCapturing = false;
                state->swallowRecorderKeyUp = true;
                refreshTopmostRecorder(GetParent(hwnd), state);
                return 0;
            }
            if (isModifierVirtualKey(key)) {
                // 只按了修饰键：先把组合显示出来，等主键按下再提交
                const std::wstring pending =
                    hotkeyModifiersPrefix(pressedModifiers()) + L"\u2026";
                SetWindowTextW(hwnd, pending.c_str());
                return 0;
            }
            const std::string name =
                platform::win32::virtualKeyToKeyName(key);
            if (name.empty()) {
                return 0;  // 不认识的键：忽略，继续等
            }
            state->topmostHotkey.enabled = true;
            state->topmostHotkey.modifiers = pressedModifiers();
            state->topmostHotkey.key = name;
            state->topmostCapturing = false;
            state->swallowRecorderKeyUp = true;
            refreshTopmostRecorder(GetParent(hwnd), state);
            return 0;
        }

        case WM_KILLFOCUS:
            state->topmostCapturing = false;
            break;

        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// 切换分页：每页的控件各管各的，互不重叠。
void switchSettingsPage(HWND dialog, SettingsDialogState* state,
                        const int page) {
    if (state == nullptr) {
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
    for (const HWND control : state->toolsControls) {
        ShowWindow(control, page == 5 ? SW_SHOW : SW_HIDE);
    }

    // 自绘标签不靠勾选态，切页后直接重画标签；卡片高度按页变化，整窗也重画。
    InvalidateRect(GetDlgItem(dialog, kAppearanceTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kBehaviorTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kPluginsTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kRadialTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kRadialPluginsTabId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kToolsTabId), nullptr, TRUE);
    InvalidateRect(dialog, nullptr, TRUE);

    if (page == 2) {
        refreshPluginList(dialog, state);
    } else if (page == 3) {
        refreshRadialSummary(dialog, state);
    } else if (page == 4) {
        refreshRadialPluginList(dialog, state);
    }
}

// 画竖向分页标签：选中态为浅强调色胶囊 + 加粗文字，未选中为灰字。
void paintSettingsTab(const HDC dc, const RECT& rect, const wchar_t* text,
                      const bool active, HFONT normalFont, HFONT activeFont) {
    if (dc == nullptr || text == nullptr) {
        return;
    }

    FillRect(dc, &rect, dialog_style::backgroundBrush());

    if (active) {
        const COLORREF accentColor = dialog_style::accent();
        // 浅强调色底：强调色以 12% 混进白底
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

// --- 消息处理

// --- 跟随启动
// 数据只存草稿（state->followLaunch），确定时才交回应用层，取消整份丢弃。

// 两份清单是否一致（决定要不要把结果交回应用层）。
// FollowLaunchEntry 没有 operator==：为这里加一个会把相等语义变成 core
// 公共约定，不值当。
bool sameFollowLaunch(const std::vector<core::FollowLaunchEntry>& left,
                      const std::vector<core::FollowLaunchEntry>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        // itemId 也要比：磁贴条目与手动条目可能同名同路径，只比名字会把
        // 换了来源的编辑当成没变（来源不同启动方式也不一样）。
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

// 重建整张列表（含图标）。条目最多 32 条，全量重建比增量更新
// （删一行后所有 iItem 错位）的正确性成本低。
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
            // 取 Small（32px）图标：缩到 18px 列表比从 256 缩干净得多。
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

    // 重复添加没有意义：选中已有那行作为反馈。
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
                } else if (state->activePage == 5) {
                    cardHeight = kCardHeightTools;
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
            // 标题要靠 DefWindowProcW 处理 WM_NCCREATE 才会从 CREATESTRUCT
            // 存进去，直接 return TRUE 会让标题栏一直空着。
            break;
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }

            HFONT font = dialogFont();
            SetPropW(hwnd, kPropFont, font);

            // 分组标题字体：从系统消息字体派生（加粗、小一号），DPI 与字形
            // 自动跟随系统设置。
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

            // 分页标签选中态字体：同字号加粗。
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

            // 创建控件时顺手记进当前分页集合，切页只做显隐。
            // 分页按钮与底部确定/取消不参与显隐，创建时 sink 为空。
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

            // 按当前语言实测文字宽度给按钮定宽：布局按中文四字按钮手工排，
            // 换成英文会裁字。用同一套对话框字体现量，取「不小于原宽度」
            // 的结果，中文不动，长语言自动撑开。
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
                // 量出的是设备像素，换回 96 DPI 逻辑像素
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
            // 表单标签列宽：整页所有「标签 + 控件」行共用同一宽度才对得齐，
            // 取当前语言最长的一条一次算定。英文 "Interface language" 超过
            // 原来的 76，写死会裁字并让右侧控件错位。
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
                                    "settings.topmostHotkey",
                                    "settings.languageLabel",
                                    "settings.radial.dragThreshold",
                                    "settings.radial.cancelRadius"}) {
                formLabelW = (std::max)(formLabelW, measureText(T(key)) + 12);
            }
            // 单选框宽度：16px 圆点 + 文字 + 空隙。写死 104 时英文
            // "Double-tap Ctrl" 会裁字，第二列还压在它身上。
            auto radioWidth = [&](const wchar_t* text) -> int {
                return measureText(text) + 26;
            };

            // --- 左侧竖向标签栏（自绘，选中态由 activePage 驱动，见 WM_DRAWITEM）
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
            makeControl(L"BUTTON", T("settings.tab.tools"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kSidebarLeft,
                        kSidebarTop + (kSidebarTabHeight + kSidebarTabGap) * 5,
                        kSidebarWidth, kSidebarTabHeight, kToolsTabId);

            sink = &state->appearanceControls;
            // 单列表单：外观页的分组自上而下排成一列。
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

            // --- 外观页：背景。背景组管窗口这一层（皮肤、背景图、整体不透明度），
            // 磁贴组管每块磁贴的底色与浓度，两者叠加成最终画面，相邻摆放方便对照。

            makeSection(T("settings.section.background"), kSectionAppearanceId, kContentLeft, yLeft,
                        false);

            // 皮肤下拉：选项来自 themes\*.json 枚举；目录被删空时退回内置
            // 预置名，保证下拉有得选（选了不存在的名字，应用层回落默认配色）。
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

            // 背景图：选图 / 清除。不显示当前文件名（随机串无信息量），
            // 主窗口一眼可见背景效果。
            makeLabel(T("settings.backgroundImage"), kContentLeft + 8, yLeft + 5, formLabelW - 8, 18);
            // 宽度按实测文字定，后面的「清除」跟着右移，不写死偏移。
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

            // --- 窗口不透明度：分层窗口与桌面合成时统一乘的系数，连搜索框、
            // 标签条、边框一起变透。与磁贴不透明度（透背景图，窗口整体不透明）
            // 是两回事。用滑块：边拖边看的观感项，输数字再看效果没有意义。
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

            // --- 磁贴

            makeSection(T("settings.section.tile"), kSectionTileId, kContentLeft, yLeft, true);

            // --- 背景色：全部磁贴的默认底色，优先级低于单项自选。色块即按钮
            // （SS_NOTIFY），点击弹内置色表；「跟随皮肤」= 清空。
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

            // --- 不透明度（磁贴）
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

            // --- 图标与标签

            makeSection(T("settings.section.iconLabel"), kSectionIconLabelId, kContentLeft,
                        yLeft, true);

            // --- 图标大小：图标相对磁贴内框的缩放（50~100），格子尺寸不变；
            // 单个磁贴可在「修改项目」里覆盖。
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

            // --- 文字标签（三态）：合并了「总闸 + 默认行为」两层开关，配置
            // 字段不变。始终显示 / 移动时显示都置 showTileLabels = true，
            // 区别在 alwaysShowTileLabels；始终不显示则 showTileLabels = false。
            makeLabel(T("settings.textLabel"), kContentLeft + 8, yLeft + 3,
                      formLabelW - 8, 18);
            {
                // 三条并排，宽度各按自己的文字实测，写死偏移会被长语言撞上。
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

            // 初值：先看总闸，再看常驻与否。隐藏状态下两项都勾不上。
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

            // --- 外观页：布局
            makeSection(T("settings.section.layout"), kSectionLayoutId, kColumnRightX, yLeft, true);

            // 列数 / 行数并排。调节器用 UDS_ALIGNRIGHT 吸在编辑框右侧内部，
            // 编辑框宽度要给箭头留约 17px。标签 x 按实测文字递推，
            // 写死会互相压住。
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

            // 提示行（灰色小字）。ID 必须显式给：makeLabel 一律用 -1，
            // 而 WM_CTLCOLORSTATIC 靠 ID 判定画灰。
            HWND hint = makeControl(
                L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kColumnRightX + 8, yLeft, kColumnWidth - 8, 18, kHintId);
            SetPropW(hwnd, kPropHint, hint);
            yLeft += 24;

            // 标签栏位置：纯几何平移，不重排磁贴，切换零风险。
            makeLabel(T("settings.tabBarPosition"), kColumnRightX + 8, yLeft + 3,
                      formLabelW - 8, 18);
            HWND tabTop = nullptr;
            HWND tabBottom = nullptr;
            {
                // 第二个单选的 x 由第一个的实测宽度递推，不写死偏移。
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

            // --- 行为页：窗口。「外观」答长什么样，「行为」答在哪儿、怎么出现。
            sink = &state->behaviorControls;
            int yRight = kContentTop;

            // --- 行为页：语言。切换后立刻落盘并重取词，本窗口下次打开才变
            // （已建好的控件不会自己改字）。
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

            // 出现位置：下拉。展示顺序与枚举值刻意不同（见 kPlacementOptions），
            // 取值经映射表，不拿 CB_GETCURSEL 当枚举用。
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

            // --- 启动与唤醒

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

            // --- 跟随启动（与开机自启是一件事的两半，紧挨着放）
            {
                HWND followLabel = makeLabel(
                    T("settings.followLaunch"), kColumnRightX + 8, yRight + 4,
                    formLabelW - 8, 18);
                addTooltip(hwnd, followLabel, T("settings.followLaunch.tip"));

                // 按钮贴右边界，宽度按实测文字定，从右往左推保持右边缘齐。
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

                    // 图标表随对话框销毁（WM_DESTROY 里 Destroy）。
                    state->followIcons = ImageList_Create(
                        scaleFor(hwnd, kFollowIconSize),
                        scaleFor(hwnd, kFollowIconSize), ILC_COLOR32 | ILC_MASK,
                        1, static_cast<int>(core::kMaximumFollowLaunchItems));
                    if (state->followIcons != nullptr) {
                        ListView_SetImageList(followList, state->followIcons,
                                              LVSIL_SMALL);
                    }

                    // 名称列宽实测，路径列吃掉剩下的宽度。
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

            // 唤醒快捷键（2x2 单选）。前两项是系统级注册热键（零开销），
            // 后两项是手势，要装低级键盘钩子（RegisterHotKey 表达不了双击/单键）。
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
                // 列宽取该列最长的一条（实测）：写死 104 宽 + 98 偏移时英文
                // "Double-tap Ctrl" 裁字还压到右列。
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
                // 单按 Win 被接管，「Ctrl+Win 仍开开始菜单」的补偿写在界面上，
                // 否则用户会以为开始菜单坏了。
                makeLabel(T("settings.winKeyNote"),
                          kColumnRightX + 8, yRight + 48,
                          kColumnWidth - 8, 18);
            }
            yRight += 66;

            // --- 插件管理页。不读写配置，只通过应用层回调管理磁盘上的插件
            // 目录：安装 = 复制 plugin.json 所在文件夹，删除 = 移除目录，
            // 添加磁贴 = 挂到当前标签页。
            sink = &state->pluginControls;

            // 一句话说明插件目录位置，完整路径太长不硬塞，
            // 点「打开目录」能看到真实位置。
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
                // 整行选中 + 双缓冲：消掉 Win7 上拖滚动条时偶发的白条闪烁。
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

            // 操作按钮一行排开。宽度按实测文字定、x 跟着前一个递推，
            // 写死会把英文裁字。
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

            // --- 轮盘菜单页：只放「什么时候呼出」，扇区动作与按应用匹配在
            // radial_editor_dialog 里编辑。控件上只写名字，解释全部挂 tooltip。
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

            // --- 一行三组：触发键 / 修饰键 / 抑制键，描述同一件事。
            // 列位置与下拉宽度按实测标签、选项宽度依次排下来
            // （见 kRadialLabelWidthTight 一带的注释）。
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

            // 抑制键：按住时轮盘一律不弹，鼠标事件原样放行。可勾选的下拉
            // （自绘 + 列表子类化，见 suppressListProc），标签带 ID 随修饰键变灰。
            const int suppressLabelW = labelWidth(T("settings.radial.suppress"));
            makeControl(L"STATIC", T("settings.radial.suppress"), WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                        triggerX, yRadial + 3,
                        suppressLabelW, 18, kRadialSuppressLabelId);
            triggerX += suppressLabelW;
            // 抑制键下拉按内容定宽：吃掉行尾剩余宽度会显得界面被横向拉开
            // （框里只写两个字，剩下一大片空白），量最长选项 + 勾选框 + 箭头即可。
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

            // 外甩取消半径：拖出这个半径再松手即取消，不触发任何动作。
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

            // 宽度按实测文字定，写死会裁掉省略号。
            makeControl(L"BUTTON", T("settings.radial.edit"),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                        kContentLeft + 8, yRadial,
                        fitWidth(T("settings.radial.edit"), 118), 26,
                        kRadialEditButtonId);

            // 悬停提示（原先印在页面上的说明文字，改成想知道才看）
            addTooltip(hwnd, radialEnabled, T("settings.radial.tip.enable"));
            addTooltip(hwnd, radialButtonCombo, T("settings.radial.tip.triggerKey"));
            addTooltip(hwnd, radialModifierCombo, T("settings.radial.tip.modifier"));
            addTooltip(hwnd, thresholdSlider, T("settings.radial.tip.dragThreshold"));
            addTooltip(hwnd, cancelSlider, T("settings.radial.tip.cancelRadius"));
            addTooltip(hwnd, suppressCombo, T("settings.radial.tip.suppress"));
            addTooltip(hwnd, radialSummary, T("settings.radial.tip.summary"));

            // 初始可用性：修饰键若是 Ctrl / Shift / Alt，抑制键一进来就该是灰的
            refreshRadialSuppressEnabled(hwnd);

            // --- 轮盘插件页（只读清单）：展示加载情况与跳过原因，一键打开
            // JSON。做成表格编辑器会把「加个目录就是加个应用」退化掉。
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
                // INFOTIP：跳过原因往往比「说明」列宽，悬停能看到全文。
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

            // --- 附加小工具页：零散系统能力，各占一节。置顶快捷键放这里
            // 而不是行为页：行为页回答「窗口在哪儿、怎么出现」，置顶是额外
            // 能力，混在一起把该页撑得过长。
            sink = &state->toolsControls;
            int yTools = kContentTop;
            makeSection(T("settings.section.tools"), kSectionToolsId,
                        kContentLeft, yTools, false);
            makeLabel(T("settings.topmostHotkey"), kContentLeft + 8,
                      yTools + 5, formLabelW - 8, 18);
            {
                // 宽度按最长的提示文字实测：录制中是「请按下快捷键…」，
                // 定格后是组合键本身，两者都放得下。
                const int recorderWidth =
                    fitWidth(T("settings.hotkey.pressKeys"), 180);
                HWND recorder = makeControl(
                    L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    0, kContentLeft + formLabelW, yTools, recorderWidth,
                    kButtonHeight, kTopmostRecorderId);
                if (recorder != nullptr) {
                    SetWindowTextW(
                        recorder,
                        topmostHotkeyDisplayText(state->topmostHotkey).c_str());
                    SetWindowSubclass(recorder, topmostRecorderProc, 1,
                                      reinterpret_cast<DWORD_PTR>(state));
                    addTooltip(hwnd, recorder,
                               T("settings.topmostHotkey.tip"));
                }
            }
            yTools += 32;
            makeControl(L"STATIC", T("settings.topmostHotkey.hint"),
                        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kContentLeft,
                        yTools, kColumnWidth, 40, -1);

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
            for (const HWND control : state->toolsControls) {
                ShowWindow(control, SW_HIDE);
            }

            // --- 底部按钮（贴右下角），不属于任何分页。
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
            // 插件列表：选中变化决定按钮可用性，双击 = 添加到当前标签页。
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr || state == nullptr) {
                break;
            }
            // 轮盘插件列表：双击 = 打开那份 JSON。
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
                 draw->CtlID == static_cast<UINT>(kRadialPluginsTabId) ||
                 draw->CtlID == static_cast<UINT>(kToolsTabId))) {
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
                } else if (draw->CtlID == static_cast<UINT>(kToolsTabId)) {
                    tabPage = 5;
                }
                const bool active =
                    state != nullptr && state->activePage == tabPage;
                paintSettingsTab(
                    draw->hDC, draw->rcItem, text, active,
                    static_cast<HFONT>(GetPropW(hwnd, kPropFont)),
                    static_cast<HFONT>(GetPropW(hwnd, kPropTabFont)));
                // 鼠标点击后系统置 ODS_NOFOCUSRECT，此时不画虚线框；
                // 只有键盘 Tab 过来才画，与原生按钮一致。
                if ((draw->itemState & ODS_FOCUS) != 0 &&
                    (draw->itemState & ODS_NOFOCUSRECT) == 0) {
                    RECT focus = draw->rcItem;
                    InflateRect(&focus, -2, -2);
                    DrawFocusRect(draw->hDC, &focus);
                }
                return TRUE;
            }

            // 抑制键下拉：字段（itemID == -1）画汇总，列表行画勾选方格。
            // N 选多没有「当前选中项」概念，原生下拉表达不了。
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

            // 磁贴背景色样本：SS_OWNERDRAW 的 STATIC 在 WM_DRAWITEM 里是
            // ODT_STATIC（不是 ODT_BUTTON），按 CtlID 判定。
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

            // 回车 / Esc 固定发 IDOK / IDCANCEL，映射到本对话框的自有 ID，
            // 否则落空（owner-draw 按钮也没有默认按钮推断可用）。
            if (controlId == IDOK) {
                controlId = kOkId;
            } else if (controlId == IDCANCEL) {
                controlId = kCancelId;
            }

            // --- 分页切换
            if ((controlId == kAppearanceTabId ||
                 controlId == kBehaviorTabId ||
                 controlId == kPluginsTabId ||
                 controlId == kRadialTabId ||
                 controlId == kRadialPluginsTabId ||
                 controlId == kToolsTabId) &&
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
                } else if (controlId == kToolsTabId) {
                    page = 5;
                }
                switchSettingsPage(hwnd, state, page);
                return 0;
            }

            // --- 置顶快捷键录制：点一下进入录制，键盘交给子类处理
            if (controlId == kTopmostRecorderId && notification == BN_CLICKED) {
                if (state != nullptr && !state->topmostCapturing) {
                    state->topmostCapturing = true;
                    SetWindowTextW(GetDlgItem(hwnd, kTopmostRecorderId),
                                   T("settings.hotkey.pressKeys"));
                    SetFocus(GetDlgItem(hwnd, kTopmostRecorderId));
                }
                return 0;
            }

            // --- 轮盘插件页：只读，动作全部转交应用层
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

            // --- 轮盘：修饰键变化决定抑制键是否可用。
            // 不改草稿：保存时统一从控件读，这里只把界面调成能表达的样子。
            if (controlId == kRadialModifierComboId &&
                notification == CBN_SELCHANGE) {
                refreshRadialSuppressEnabled(hwnd);
                return 0;
            }

            // --- 跟随启动：添加 / 移除，只改草稿，确定时才交回。
            if (controlId == kFollowAddId && notification == BN_CLICKED) {
                if (state != nullptr && addFollowEntry(hwnd, state)) {
                    refreshFollowList(hwnd, state);
                    // 选中并滚到刚加的那条，否则看不到反馈。
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

            // --- 轮盘：打开扇区动作编辑器。子对话框直接改草稿 state->radial，
            // 返回 true（点了确定）才刷新概况行。
            if (controlId == kRadialEditButtonId &&
                notification == BN_CLICKED) {
                if (state != nullptr) {
                    if (showRadialEditorWindow(hwnd, &state->radial)) {
                        refreshRadialSummary(hwnd, state);
                    }
                }
                return 0;
            }

            // --- 插件管理：安装（选择 plugin.json → 复制整个目录）
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

            // --- 插件管理：添加磁贴 / 打开目录 / 删除
            if ((controlId == kPluginAddTileId ||
                 controlId == kPluginOpenFolderId ||
                 controlId == kPluginRemoveId) &&
                notification == BN_CLICKED) {
                if (state == nullptr || state->plugins == nullptr) {
                    return 0;
                }
                const std::wstring id = selectedPluginId(hwnd, state);
                if (controlId == kPluginOpenFolderId) {
                    // 没选中时打开插件根目录，方便手工往里放文件夹。
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

            // --- 皮肤：切换即预览。不落盘、不改 state->result，提交只看
            // 「确定」那一刻；取消时由调用方重新套用配置里的皮肤。
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

            // --- 标签栏位置：选中即预览。radio 互斥，按 controlId 判定方向。
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

            // --- 背景图：选择图片
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
                        // 立即贴到主窗口预览，先看效果再决定是否确定
                        if (state->preview != nullptr &&
                            state->preview->backgroundChanged) {
                            state->preview->backgroundChanged(
                                state->pickedBackgroundPath);
                        }
                    }
                }
                return 0;
            }

            // --- 背景图：清除
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

            // --- 磁贴背景色：选色 / 恢复跟随皮肤，只通告预览不写 result。
            // STATIC 带 SS_NOTIFY 才发 STN_CLICKED；双击先给 STN_CLICKED 再给
            // STN_DBLCLK，只认前者，避免连开两个调色板。
            if (controlId == kDefaultTileColorSwatchId &&
                notification == STN_CLICKED) {
                if (state != nullptr) {
                    // 初始色：已选则用它；否则用皮肤磁贴底的近似值，
                    // 让调色板勾在用户当前看到的那一格附近。
                    const std::uint32_t initial =
                        state->chosenDefaultTileColor.value_or(0xFF3A3A3EU);
                    // 内置色表：每点一格立刻刷新预览，弹板不关，方便连续换色。
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

                    // 只记录真的改了的项，调用方据此跳过落盘与布局重建。
                    if (columns != static_cast<int>(state->initialColumns)) {
                        state->result.gridColumns =
                            static_cast<std::uint16_t>(columns);
                    }
                    if (rows != static_cast<int>(state->initialRows)) {
                        state->result.gridRows =
                            static_cast<std::uint16_t>(rows);
                    }

                    // --- 文字标签（三态单选）
                    // 隐藏时不改 alwaysShowTileLabels：保留原值，用户切回
                    // 「始终显示」/「移动时显示」时回到原来的选择。
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

                    // --- 语言：与初值不同才回传，值可以是 "auto"（跟随系统）。
                    {
                        const std::string language = selectedLanguageCode(hwnd);
                        if (language != state->initialLanguage) {
                            state->result.language = language;
                        }
                    }

                    // --- 标签栏位置（单选）
                    HWND tabBottom = GetDlgItem(hwnd, kTabBottomRadioId);
                    if (tabBottom != nullptr) {
                        const bool checked =
                            SendMessageW(tabBottom, BM_GETCHECK, 0, 0) ==
                            BST_CHECKED;
                        if (checked != state->initialTabBarAtBottom) {
                            state->result.tabBarAtBottom = checked;
                        }
                    }

                    // --- 窗口出现位置（下拉索引 → 映射表）
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

                    // --- 皮肤
                    const std::wstring themeName = ui::canonicalThemeName(
                        comboSelectedText(hwnd, kThemeComboId));
                    if (!themeName.empty() &&
                        themeName != state->initialThemeName) {
                        state->result.themeName =
                            base::wideToUtf8Lossy(themeName);
                    }

                    // --- 背景图：清除优先于新选（都点了以清除为准）
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

                    // --- 置顶快捷键（录制控件直接改草稿，与初值比较）
                    if (!sameHotkeyConfig(state->topmostHotkey,
                                          state->initialTopmostHotkey)) {
                        state->result.topmostHotkey = state->topmostHotkey;
                    }

                    // 跟随启动：整份交回，顺序也算改动。
                    if (!sameFollowLaunch(state->followLaunch,
                                          state->initialFollowLaunch)) {
                        state->result.followLaunch = state->followLaunch;
                    }

                    // --- 唤醒快捷键（单选组，找出被选中的那个）
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

                    // --- 不透明度（只有真正拖动过才记录）。磁贴背景的初值是
                    // 皮肤实际的 alpha，没碰滑块就不落盘，否则「跟随皮肤」被
                    // 写死成数字，换皮肤不再跟随。
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

                    // --- 磁贴背景色：必须用 touched 而不是单看值，内层空值
                    // （取消选色、回到跟随皮肤）也是有效改动。
                    if (state->defaultTileColorTouched &&
                        state->chosenDefaultTileColor !=
                            state->initialDefaultTileColor) {
                        state->result.defaultTileColor =
                            state->chosenDefaultTileColor;
                    }

                    // --- 轮盘菜单：触发方式在这里从控件读，扇区动作与按应用
                    // 规则已在 state->radial 草稿里。整份交回，应用层落盘并
                    // 重装钩子。
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
                        // 抑制键：从 item data 重建，与触发修饰键撞车的丢掉，
                        // 保证交给 core 的配置必然合法。
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
            // 上下调节器会改写编辑框内容，其事件也一并覆盖。
            if ((controlId == kColumnsEditId || controlId == kRowsEditId)) {
                if (notification == EN_CHANGE) {
                    refreshHint(hwnd, state);
                    if (state != nullptr && state->preview != nullptr &&
                        state->preview->gridChanged) {
                        // 读值夹到合法范围但不回写控件：中途改写会把
                        // 用户输入吃掉。
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

            // 文字标签三态单选：选中即预览。同时推出两个布尔量：
            // show = 不是「始终不显示」，always = 选的是「始终显示」。
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

            // 上下箭头按下时 EN_CHANGE 未必立即到达，补一次刷新。
            if (controlId == kColumnsSpinId || controlId == kRowsSpinId) {
                refreshHint(hwnd, state);
                return 0;
            }
            break;
        }

        case WM_HSCROLL: {
            // trackbar 走 WM_HSCROLL，与 WM_COMMAND 是两条通知路径。
            const HWND control = reinterpret_cast<HWND>(lParam);
            if (control == nullptr) {
                break;
            }
            const int controlId = GetDlgCtrlID(control);

            // 轮盘页的两个像素滑块：拖动时只刷新数值文字，模型在「确定」时
            // 统一从控件读一次（单一来源，不会出现「拖过但没读」的中间态）。
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
            // STATIC 统一卡片白底 + 正文色，提示行用次级灰。返回的画刷必须
            // 与卡片一致，否则文字后面出现一块异色。
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
            // 单选框 / 复选框默认用 COLOR_BTNFACE 填底，在白卡片上是灰方块，
            // 统一改成卡片底色。
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dialog_style::text());
            SetBkColor(dc, dialog_style::card());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_CLOSE:
            // 关闭等同于取消，避免「关掉窗口但改动被默默套用」
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

            // 图标表必须显式销毁：列表控件不回收句柄，否则每次漏一批 HICON。
            if (state != nullptr && state->followIcons != nullptr) {
                ImageList_Destroy(state->followIcons);
                state->followIcons = nullptr;
            }

            // 通知模态循环本对话框已关闭。不用 PostQuitMessage：线程级且
            // 粘性的 WM_QUIT 会连带退出主消息循环，表现为点确定整个程序退出。
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

// 把设置窗口摆到主窗口外侧：右 → 左 → 下 → 上。coverCenterFallback = true
// （首次打开）时四面放不下就居中覆盖；= false（网格预览中重新摆位）时主窗口
// 正在变化，居中会被盖住，退到工作区右下角保证自身可见。
// 用当前窗口矩形（含标题栏边框）算尺寸，DPI 与边框厚度自动正确。
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
                // 网格预览中：躲到工作区右下角，不压主窗口
                x = work.right - width - gap;
                y = work.bottom - height - gap;
                if (x < work.left) x = work.left;
                if (y < work.top) y = work.top;
                placed = true;
            }

            if (placed) {
                // 夹进工作区：主窗口贴屏幕边缘时算出的位置可能一半在屏外
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

// --- 对外入口

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
                        const core::HotkeyConfig& currentTopmostHotkey,
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

    // comctl32 公共控件（上下调节器、列表）未初始化的话补一次，重复调用无害。
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
    state.initialTopmostHotkey = currentTopmostHotkey;
    state.topmostHotkey = currentTopmostHotkey;
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

    // --- 尺寸：客户区按设计稿，换算成含标题栏边框的窗口尺寸
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

    // --- 定位：设置窗口打开期间主窗口保持可见，各项设置要实时看到效果，
    // 优先排在主窗口外侧。
    positionSettingsWindow(dialog, owner, true);

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环：对话框消息走 IsDialogMessage（Tab / 回车触发默认按钮），
    // 其余照常派发，托盘与热键在对话框打开时仍然工作。
    // 退出条件用 IsWindow(dialog)，不依赖 GetMessage 返回 0：那需要 WM_QUIT，
    // 会连带杀掉主消息循环。kDialogCloseMessage 只负责唤醒循环。
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
