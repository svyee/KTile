#include "ui/settings_window.h"

#include "base/text_convert.h"
#include "core/data_model.h"
#include "ui/theme.h"

#include <commctrl.h>
#include <commdlg.h>

#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// ---------------------------------------------------------------- 布局常量
//
// 全部按 96 DPI 的「逻辑像素」书写，运行时统一用 scaleFor() 缩放。
// 这样做的好处：换一台 150% 缩放屏时不必重算任何数字，只需保证
// 这里写的是设计稿尺寸。

constexpr int kDialogWidth = 330;
// 高度按四个分组（外观 / 布局 / 窗口 / 启动与唤醒）累加得出：
//   外观     皮肤(26) + 背景图(27) + 整体不透明度(30) + 磁贴背景(30)
//            + 文字标签(24) + 默认行为(26)
//   布局     网格(24+4) + 提示(24) + 标签栏位置(26)
//   窗口     出现位置(26)
//   启动与唤醒 开机自启(26) + 唤醒方式(62)
// 每个分组的标题另占 24，非首组再加 14 的分隔线间距。
// 改任何一行的行高都要回来重算这个数，否则底部按钮会压到内容上。
constexpr int kDialogHeight = 556;

constexpr int kMargin = 16;          // 左右页边距
constexpr int kRowGap = 12;          // 行间垂直间距
constexpr int kLabelWidth = 76;      // 「列数」等标签宽度
constexpr int kSpinnerWidth = 96;    // 数字输入框宽度
constexpr int kSpinnerHeight = 24;
constexpr int kGroupGap = 18;        // 分组之间的额外间隔
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
struct PlacementOption {
    const wchar_t* label;
    core::WindowPlacement value;
};

constexpr PlacementOption kPlacementOptions[] = {
    {L"跟随鼠标", core::WindowPlacement::Mouse},
    {L"屏幕中央", core::WindowPlacement::Center},
    {L"左下角", core::WindowPlacement::LeftBottom},
    {L"右下角", core::WindowPlacement::RightBottom},
    {L"左上角", core::WindowPlacement::LeftTop},
    {L"右上角", core::WindowPlacement::RightTop},
    {L"上一次的位置", core::WindowPlacement::Last},
};

constexpr int kPlacementOptionCount =
    static_cast<int>(sizeof(kPlacementOptions) / sizeof(kPlacementOptions[0]));

// ---------------------------------------------------------------- 控件 ID

constexpr int kColumnsEditId = 1101;
constexpr int kRowsEditId = 1102;
constexpr int kColumnsSpinId = 1103;
constexpr int kRowsSpinId = 1104;
constexpr int kLabelsCheckId = 1110;
constexpr int kTitleAlwaysRadioId = 1118;   // 默认行为：一直显示
constexpr int kTitleDynamicRadioId = 1119;  // 默认行为：移入时显示
constexpr int kTabTopRadioId = 1121;        // 标签栏位置：顶部
constexpr int kTabBottomRadioId = 1122;     // 标签栏位置：底部
constexpr int kThemeComboId = 1114;
constexpr int kBackgroundBrowseId = 1115;
constexpr int kBackgroundClearId = 1116;
constexpr int kBackgroundLabelId = 1117;
constexpr int kWindowPlacementComboId = 1140;
constexpr int kWindowOpacitySliderId = 1150;    // 整体不透明度滑块
constexpr int kWindowOpacityValueId = 1151;     // 整体不透明度数值
constexpr int kTileOpacitySliderId = 1152;      // 磁贴背景不透明度滑块
constexpr int kTileOpacityValueId = 1153;       // 磁贴背景不透明度数值
constexpr int kHotkeyAltSpaceRadioId = 1141;    // 唤醒快捷键：Alt+Space
constexpr int kHotkeyCtrlSpaceRadioId = 1142;   // 唤醒快捷键：Ctrl+Space
constexpr int kHotkeyDoubleCtrlRadioId = 1143;  // 唤醒快捷键：双击 Ctrl
constexpr int kHotkeyWinKeyRadioId = 1144;      // 唤醒快捷键：单按 Win
constexpr int kStartupCheckId = 1111;
constexpr int kHintId = 1120;
constexpr int kOkId = 1130;
constexpr int kCancelId = 1131;

// 分组标题的控件 ID。用一段连续区间而不是逐个常量，是为了在
// WM_CTLCOLORSTATIC 里一次判定「这批是分组标题，画成灰色」。
constexpr int kSectionHeaderIdBase = 1201;
constexpr int kSectionAppearanceId = 1201;
constexpr int kSectionLayoutId = 1202;
constexpr int kSectionWindowId = 1203;
constexpr int kSectionStartupId = 1204;
constexpr int kSectionHeaderIdEnd = 1210;

// 控件 ID 到「窗口属性名」的映射：把控件句柄挂在对话框上，
// 避免到处 GetDlgItem（虽然也能用，但属性查找更直白）。
constexpr const wchar_t* kPropColumnsEdit = L"KTileSetColumnsEdit";
constexpr const wchar_t* kPropRowsEdit = L"KTileSetRowsEdit";
constexpr const wchar_t* kPropHint = L"KTileSetHint";
constexpr const wchar_t* kPropFont = L"KTileSetFont";
constexpr const wchar_t* kPropSectionFont = L"KTileSetSectionFont";

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

    // 皮肤与背景图
    std::wstring initialThemeName;      // 当前皮肤显示名（空 = 默认）
    std::wstring initialBackgroundName; // 当前背景图文件名（仅显示）
    std::wstring pickedBackgroundPath;  // 本次会话新选的图片（绝对路径）
    bool backgroundCleared = false;     // 用户点了「清除」

    // 当前布局已占用的行列数（用于提示文案）
    std::uint16_t columnsInUse = 0;
    std::uint16_t rowsInUse = 0;

    // 实时预览回调（不拥有；指向 showSettingsWindow 栈上的对象）
    const SettingsPreviewHooks* preview = nullptr;

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
        text = L"提示：网格小于当前布局占用，部分磁贴会自动重排。";
    } else {
        text = L"网格范围 3 - 16。窗体会随格数自动调整大小。";
    }
    SetWindowTextW(hint, text.c_str());
}

// ---------------------------------------------------------------- 消息处理

LRESULT CALLBACK settingsDialogProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
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
                return control;
            };

            auto makeLabel = [&](const wchar_t* text, const int x, const int y,
                                 const int w, const int h) -> HWND {
                return makeControl(L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, x, y, w, h, -1);
            };

            int y = kMargin;

            // 分组标题：加粗小一号 + 上方细分隔线（首组不画）。
            // 设置项按用途归入「外观 / 布局 / 窗口 / 启动与唤醒」四类，
            // 顺序按「改得最勤 → 一动就得重开程序」排，找一项不必逐行扫。
            auto makeSection = [&](const wchar_t* text, const int id,
                                   const bool withRule) {
                if (withRule) {
                    y += 6;
                    makeControl(L"STATIC", L"",
                                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0,
                                kMargin, y, kDialogWidth - 2 * kMargin, 2, -1);
                    y += 6;
                }
                HWND header = makeControl(
                    L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                    kMargin + 8, y, kDialogWidth - 2 * kMargin - 8, 18, id);
                if (header != nullptr && sectionFont != nullptr) {
                    SendMessageW(header, WM_SETFONT,
                                 reinterpret_cast<WPARAM>(sectionFont), TRUE);
                }
                y += 24;
            };

            // ==================== 外观 ====================

            makeSection(L"外观", kSectionAppearanceId, false);

            // 皮肤下拉：选项来自 <data>\themes\*.json（即枚举结果）。
            // 枚举为空（目录被整个删了）时退回内置预置名，保证下拉
            // 永远有得选 —— 选了不存在文件的名字，应用层会回落默认配色。
            makeLabel(L"皮肤", kMargin + 8, y + 5, kLabelWidth - 8, 18);
            HWND themeCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, kMargin + kLabelWidth, y,
                kDialogWidth - kMargin * 2 - kLabelWidth, 160,
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
                    SendMessageW(themeCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(
                                     entries[i].displayName.c_str()));
                    if (entries[i].displayName == state->initialThemeName) {
                        selectIndex = static_cast<int>(i);
                    }
                }
                SendMessageW(themeCombo, CB_SETCURSEL,
                             selectIndex >= 0 ? selectIndex : 0, 0);
            }
            y += 26;

            // 背景图：选图 / 清除 / 当前文件名。
            // 路径太长会把这一行挤爆，所以只显示文件名（SS_PATHELLIPSIS
            // 再兜一层，超长仍然省略中间）。
            makeLabel(L"背景图", kMargin + 8, y + 5, kLabelWidth - 8, 18);
            makeControl(
                L"BUTTON", L"选择图片...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, kMargin + kLabelWidth, y, 82, 23,
                kBackgroundBrowseId);
            makeControl(
                L"BUTTON", L"清除",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, kMargin + kLabelWidth + 88, y, 52, 23,
                kBackgroundClearId);
            {
                const std::wstring currentText =
                    state->initialBackgroundName.empty()
                        ? std::wstring(L"未设置")
                        : state->initialBackgroundName;
                makeControl(
                    L"STATIC", currentText.c_str(),
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                    0, kMargin + kLabelWidth + 148, y + 5,
                    kDialogWidth - kMargin * 2 - kLabelWidth - 148, 18,
                    kBackgroundLabelId);
            }
            y += 27;

            // ---- 不透明度 ----
            //
            // 两个旋钮不是一回事，界面上刻意分开摆：
            //   · 整体不透明：窗口与桌面合成时统一乘的系数（分层窗口），
            //     连搜索框、标签条、边框一起变透，透出来的是**桌面**；
            //   · 磁贴背景  ：磁贴底色自己画多浓，透出来的是**背景图**，
            //     窗口整体仍然不透明。
            // 用滑块而不是数字框：这两项都是「边拖边看」的观感调整，
            // 输完数字才知道效果就失去意义了。
            makeLabel(L"整体不透明", kMargin + 8, y + 5, kLabelWidth - 8, 18);
            HWND windowOpacitySlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                0, kMargin + kLabelWidth, y - 2, 156, 26,
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
                            kMargin + kLabelWidth + 162, y + 5, 44, 18,
                            kWindowOpacityValueId);
            }
            y += 30;

            makeLabel(L"磁贴背景", kMargin + 8, y + 5, kLabelWidth - 8, 18);
            HWND tileOpacitySlider = makeControl(
                TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                0, kMargin + kLabelWidth, y - 2, 156, 26,
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
                            kMargin + kLabelWidth + 162, y + 5, 44, 18,
                            kTileOpacityValueId);
            }
            y += 30;

            // 文字标签总闸。勾掉后「默认行为」整组置灰（不是隐藏）——
            // 位置固定，界面不跳。
            HWND labels = makeControl(
                L"BUTTON", L"显示磁贴文字标签",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, kMargin + 8, y,
                kDialogWidth - 2 * kMargin - 8, 20, kLabelsCheckId);
            if (labels != nullptr) {
                SendMessageW(labels, BM_SETCHECK,
                             state->initialShowLabels ? BST_CHECKED : BST_UNCHECKED,
                             0);
            }
            y += 24;

            // 默认行为：单选（一直显示 / 移入时显示）。
            // 与上面是两层开关（总闸 / 常驻与否），跟随总闸置灰。
            makeLabel(L"默认行为", kMargin + 8, y + 3, kLabelWidth - 8, 18);
            HWND titleAlways = makeControl(
                L"BUTTON", L"一直显示",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                    BS_AUTORADIOBUTTON,
                0, kMargin + kLabelWidth, y, 78, 20, kTitleAlwaysRadioId);
            if (titleAlways != nullptr) {
                SendMessageW(titleAlways, BM_SETCHECK,
                             state->initialAlwaysShowTitle ? BST_CHECKED
                                                           : BST_UNCHECKED,
                             0);
            }
            HWND titleDynamic = makeControl(
                L"BUTTON", L"移入时显示",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                0, kMargin + kLabelWidth + 82, y, 96, 20,
                kTitleDynamicRadioId);
            if (titleDynamic != nullptr) {
                SendMessageW(titleDynamic, BM_SETCHECK,
                             state->initialAlwaysShowTitle ? BST_UNCHECKED
                                                           : BST_CHECKED,
                             0);
                const BOOL enabled =
                    state->initialShowLabels ? TRUE : FALSE;
                EnableWindow(titleAlways, enabled);
                EnableWindow(titleDynamic, enabled);
            }
            y += 26;

            // ==================== 布局 ====================

            makeSection(L"布局", kSectionLayoutId, true);

            // 列数 / 行数并排：两者一起决定磁贴格数，拆成两行纯属浪费。
            // 上下调节器用 UDS_ALIGNRIGHT 吸附在编辑框右侧内部，
            // 所以编辑框宽度里要给箭头留出约 17px。
            makeLabel(L"网格", kMargin + 8, y + 3, 30, 18);
            HWND columnsEdit = makeControl(
                L"EDIT", std::to_wstring(state->initialColumns).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                WS_EX_CLIENTEDGE,
                kMargin + 56, y, 88, kSpinnerHeight, kColumnsEditId);
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

            makeLabel(L"行数", kMargin + 162, y + 3, 30, 18);
            HWND rowsEdit = makeControl(
                L"EDIT", std::to_wstring(state->initialRows).c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                WS_EX_CLIENTEDGE,
                kMargin + 192, y, 88, kSpinnerHeight, kRowsEditId);
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
            y += kSpinnerHeight + 4;

            // 提示行（灰色小字）。ID 必须显式给 —— makeLabel 一律用 -1，
            // 而 WM_CTLCOLORSTATIC 靠 ID 区分「该画灰的说明文字」。
            HWND hint = makeControl(
                L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kMargin + 8, y, kDialogWidth - 2 * kMargin - 8, 18, kHintId);
            SetPropW(hwnd, kPropHint, hint);
            y += 24;

            // 标签栏位置：单选（顶部 / 底部）。
            // 纯几何平移，不重排磁贴、不改窗口尺寸，切换零风险。
            makeLabel(L"标签栏位置", kMargin + 8, y + 3, kLabelWidth - 8, 18);
            HWND tabTop = makeControl(
                L"BUTTON", L"顶部",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                    BS_AUTORADIOBUTTON,
                0, kMargin + kLabelWidth, y, 60, 20, kTabTopRadioId);
            if (tabTop != nullptr) {
                SendMessageW(tabTop, BM_SETCHECK,
                             state->initialTabBarAtBottom ? BST_UNCHECKED
                                                          : BST_CHECKED,
                             0);
            }
            HWND tabBottom = makeControl(
                L"BUTTON", L"底部",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                0, kMargin + kLabelWidth + 64, y, 60, 20,
                kTabBottomRadioId);
            if (tabBottom != nullptr) {
                SendMessageW(tabBottom, BM_SETCHECK,
                             state->initialTabBarAtBottom ? BST_CHECKED
                                                          : BST_UNCHECKED,
                             0);
            }
            y += 26;

            // ==================== 窗口 ====================

            makeSection(L"窗口", kSectionWindowId, true);

            // 出现位置：下拉。展示顺序与 core::WindowPlacement 的枚举值
            // 刻意不同（枚举要保已存配置不错位，Center 只能追加末尾），
            // 取值时经 kPlacementOptions 映射，不拿 CB_GETCURSEL 当枚举用。
            makeLabel(L"出现位置", kMargin + 8, y + 5, kLabelWidth - 8, 18);
            HWND placementCombo = makeControl(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, kMargin + kLabelWidth, y,
                kDialogWidth - kMargin * 2 - kLabelWidth, 160,
                kWindowPlacementComboId);
            if (placementCombo != nullptr) {
                for (int i = 0; i < kPlacementOptionCount; ++i) {
                    SendMessageW(placementCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(
                                     kPlacementOptions[i].label));
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
            y += 26;

            // ==================== 启动与唤醒 ====================

            makeSection(L"启动与唤醒", kSectionStartupId, true);

            HWND startup = makeControl(
                L"BUTTON", L"开机自动启动",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP |
                    BS_AUTOCHECKBOX,
                0, kMargin + 8, y,
                kDialogWidth - 2 * kMargin - 8, 20, kStartupCheckId);
            if (startup != nullptr) {
                SendMessageW(startup, BM_SETCHECK,
                             state->initialStartup ? BST_CHECKED : BST_UNCHECKED,
                             0);
            }
            y += 26;

            // 唤醒快捷键（2x2 单选）：
            // 前两项是系统级注册热键（零开销），后两项是手势，需要装
            // 低级键盘钩子（RegisterHotKey 表达不了双击/单键）。
            // 做成预设而不是「录制组合键」：录制要处理修饰键、冲突检测、
            // 反馈文案，收益却很低 —— 这四个已经覆盖绝大多数诉求。
            makeLabel(L"唤醒方式", kMargin + 8, y + 3, kLabelWidth - 8, 18);
            {
                struct PresetRadio {
                    int id;
                    const wchar_t* label;
                    core::HotkeyPreset preset;
                    int x;
                    int y;
                };
                const PresetRadio radios[] = {
                    {kHotkeyAltSpaceRadioId, L"Alt+Space",
                     core::HotkeyPreset::AltSpace,
                     kMargin + kLabelWidth, y},
                    {kHotkeyCtrlSpaceRadioId, L"Ctrl+Space",
                     core::HotkeyPreset::CtrlSpace,
                     kMargin + kLabelWidth + 104, y},
                    {kHotkeyDoubleCtrlRadioId, L"双击 Ctrl",
                     core::HotkeyPreset::DoubleCtrl,
                     kMargin + kLabelWidth, y + 22},
                    {kHotkeyWinKeyRadioId, L"单按 Win 键",
                     core::HotkeyPreset::WinKey,
                     kMargin + kLabelWidth + 104, y + 22},
                };
                for (const auto& radio : radios) {
                    const bool checked =
                        radio.preset == state->initialHotkeyPreset;
                    HWND button = makeControl(
                        L"BUTTON", radio.label,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                            (radio.id == kHotkeyAltSpaceRadioId ? WS_GROUP : 0) |
                            BS_AUTORADIOBUTTON,
                        0, radio.x, radio.y, 104, 20, radio.id);
                    if (button != nullptr) {
                        SendMessageW(button, BM_SETCHECK,
                                     checked ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                }
                // 单按 Win 会被我们接管，所以把「Ctrl+Win 仍开开始菜单」
                // 这条补偿写在界面上，否则用户会以为开始菜单坏了。
                makeLabel(L"单按 Win 时，Ctrl+Win 仍打开开始菜单。",
                          kMargin + 8, y + 44,
                          kDialogWidth - 2 * kMargin - 8, 18);
            }
            y += 62;

            // ---- 底部按钮（贴右下角） ----
            const int buttonY = kDialogHeight - kMargin - kButtonHeight;
            makeControl(
                L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0,
                kDialogWidth - kMargin - 2 * kButtonWidth - 8, buttonY,
                kButtonWidth, kButtonHeight, kOkId);
            makeControl(
                L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0,
                kDialogWidth - kMargin - kButtonWidth, buttonY,
                kButtonWidth, kButtonHeight, kCancelId);

            refreshHint(hwnd, state);

            if (columnsEdit != nullptr) {
                SetFocus(columnsEdit);
                SendMessageW(columnsEdit, EM_SETSEL, 0, -1);
            }
            return 0;
        }

        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            const int notification = HIWORD(wParam);

            // ---- 皮肤：切换即预览 ----
            //
            // 只通知调用方「换成哪套了」，不落盘、不改 state->result ——
            // 真正的提交仍然只看「确定」那一刻选了谁。取消时由调用方
            // 重新套用配置里的皮肤，预览自动消失。
            if (controlId == kThemeComboId && notification == CBN_SELCHANGE) {
                if (state != nullptr && state->preview != nullptr &&
                    state->preview->themeChanged) {
                    const std::wstring name =
                        comboSelectedText(hwnd, kThemeComboId);
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
                    ofn.lpstrFilter =
                        L"图片文件\\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\\0"
                        L"所有文件\\0*.*\\0";
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn) != FALSE) {
                        state->pickedBackgroundPath = file;
                        state->backgroundCleared = false;
                        // 只显示文件名（路径太长会挤爆这一行）
                        std::wstring name = file;
                        const std::size_t slash =
                            name.find_last_of(L"\\/");
                        if (slash != std::wstring::npos) {
                            name = name.substr(slash + 1);
                        }
                        HWND label = GetDlgItem(hwnd, kBackgroundLabelId);
                        if (label != nullptr) {
                            SetWindowTextW(label, name.c_str());
                        }
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
                    HWND label = GetDlgItem(hwnd, kBackgroundLabelId);
                    if (label != nullptr) {
                        SetWindowTextW(label, L"未设置");
                    }
                    // 主窗口同步撤掉背景图（仍只是预览，未落盘）
                    if (state->preview != nullptr &&
                        state->preview->backgroundChanged) {
                        state->preview->backgroundChanged(std::wstring());
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

                    HWND labels = GetDlgItem(hwnd, kLabelsCheckId);
                    if (labels != nullptr) {
                        const bool checked =
                            SendMessageW(labels, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (checked != state->initialShowLabels) {
                            state->result.showTileLabels = checked;
                        }
                    }

                    // ---- 标题默认行为（单选）----
                    // 直接看「一直显示」是否被选中：radio 组内互斥，
                    // 另一项必然取反。
                    HWND titleAlways = GetDlgItem(hwnd, kTitleAlwaysRadioId);
                    if (titleAlways != nullptr) {
                        const bool checked =
                            SendMessageW(titleAlways, BM_GETCHECK, 0, 0) ==
                            BST_CHECKED;
                        if (checked != state->initialAlwaysShowTitle) {
                            state->result.alwaysShowTileLabels = checked;
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
                    const std::wstring themeName =
                        comboSelectedText(hwnd, kThemeComboId);
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

            // 总闸「显示磁贴文字标签」：点击即预览 + 联动置灰
            if (controlId == kLabelsCheckId &&
                notification == BN_CLICKED) {
                const BOOL checked =
                    SendMessageW(GetDlgItem(hwnd, kLabelsCheckId),
                                 BM_GETCHECK, 0, 0) == BST_CHECKED
                        ? TRUE
                        : FALSE;
                EnableWindow(GetDlgItem(hwnd, kTitleAlwaysRadioId), checked);
                EnableWindow(GetDlgItem(hwnd, kTitleDynamicRadioId), checked);
                if (state != nullptr && state->preview != nullptr &&
                    state->preview->showTileLabelsChanged) {
                    state->preview->showTileLabelsChanged(checked == TRUE);
                }
                return 0;
            }

            // 标题默认行为单选：选中即预览
            if ((controlId == kTitleAlwaysRadioId ||
                 controlId == kTitleDynamicRadioId) &&
                notification == BN_CLICKED) {
                if (state != nullptr && state->preview != nullptr &&
                    state->preview->alwaysShowTitleChanged) {
                    state->preview->alwaysShowTitleChanged(
                        controlId == kTitleAlwaysRadioId);
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
            if (controlId != kWindowOpacitySliderId &&
                controlId != kTileOpacitySliderId) {
                break;
            }

            const int percent =
                static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            HWND value = GetDlgItem(
                hwnd, controlId == kWindowOpacitySliderId
                          ? kWindowOpacityValueId
                          : kTileOpacityValueId);
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
                } else {
                    state->tileOpacityTouched = true;
                    if (state->preview != nullptr &&
                        state->preview->tileOpacityChanged) {
                        state->preview->tileOpacityChanged(percent);
                    }
                }
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // 分组标题与提示行画成灰色，一眼区分「标题 / 说明」与可操作
            // 的正文。背景必须交回系统画刷 + TRANSPARENT：STATIC 默认
            // 会用窗口背景色自己填一遍，一旦这里返回别的画刷就会出现
            // 一块和对话框底色不一致的方框。
            const int controlId =
                GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            const bool isSection = controlId >= kSectionHeaderIdBase &&
                                   controlId < kSectionHeaderIdEnd;
            if (isSection || controlId == kHintId) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, isSection ? RGB(96, 96, 96)
                                           : RGB(122, 122, 122));
                return reinterpret_cast<LRESULT>(
                    GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
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
            RemovePropW(hwnd, kPropColumnsEdit);
            RemovePropW(hwnd, kPropRowsEdit);
            RemovePropW(hwnd, kPropHint);

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
                        const std::string& currentBackground,
                        const bool currentStartup,
                        const core::HotkeyPreset currentHotkeyPreset,
                        const int currentWindowOpacity,
                        const int currentTileOpacity,
                        const std::uint16_t columnsInUse,
                        const std::uint16_t rowsInUse,
                        SettingsResult* out,
                        const SettingsPreviewHooks* preview) {
    if (out == nullptr) {
        return false;
    }

    // 上下调节器属于 comctl32 的「Common Controls」。
    // 主程序启动时未必初始化过，这里补一次（重复调用无害）。
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(INITCOMMONCONTROLSEX);
    controls.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
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
    // 背景图只显示文件名
    {
        std::wstring backgroundWide = base::utf8ToWide(currentBackground);
        const std::size_t slash = backgroundWide.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            backgroundWide = backgroundWide.substr(slash + 1);
        }
        state.initialBackgroundName = backgroundWide;
    }
    state.initialStartup = currentStartup;
    state.initialHotkeyPreset = currentHotkeyPreset;
    state.initialWindowOpacity = currentWindowOpacity;
    state.initialTileOpacity = currentTileOpacity;
    state.columnsInUse = columnsInUse;
    state.rowsInUse = rowsInUse;
    state.preview = preview;

    // ---- 尺寸：客户区按设计稿，换算成含标题栏边框的窗口尺寸 ----
    RECT desired{0, 0, scaleFor(owner, kDialogWidth),
                 scaleFor(owner, kDialogHeight)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    HWND dialog = CreateWindowExW(
        exStyle,
        kSettingsDialogClass,
        L"KTile 设置",
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
