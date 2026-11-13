// KTile - UI 层：轮盘菜单编辑器（实现）
//
// 界面分两页（左上角页签切换）：扇区动作页是扇区列表 + 动作表单；
// 按应用页是进程黑名单。
#include "ui/radial_editor_dialog.h"

#include "ui/builtin_labels.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "infrastructure/i18n.h"
#include "ui/dialog_style.h"
#include "ui/radial_labels.h"

#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// --- 常量

constexpr const wchar_t* kEditorClass = L"KTile.RadialEditor";

// 对话框关闭通知（唤醒本对话框的嵌套模态循环，语义与设置窗口一致）
constexpr UINT kEditorCloseMessage = WM_APP + 0x91U;

constexpr int kDialogWidth = 700;
// 高度按最密的一页（扇区动作：列表 + 四行动作表单）定，两页共用；
// 460 时提示文字会盖住「确定」按钮，往下让 40。
constexpr int kDialogHeight = 500;
constexpr int kMargin = 16;
constexpr int kButtonWidth = 78;
constexpr int kButtonHeight = 26;
constexpr int kSmallButtonHeight = 24;

// 顶部页签
constexpr int kTabLeft = 16;      // 两枚页签的起始横坐标

// 右列（扇区列表 + 动作表单）。左侧方案列表随多方案一起删掉，
// 宽度让给内容区。
constexpr int kRightLeft = kMargin;
constexpr int kRightWidth = kDialogWidth - kMargin * 2;  // 668
// 列表从页签下方的「扇区数」一行之后开始；190 够放 8 个扇区（每行约 22）
constexpr int kSectorListTop = 82;
constexpr int kSectorListHeight = 190;

// 动作表单：四行网格。标签列按当前语言实测宽度（见 createControls），
// 控件列共用左边界 fieldX，右边缘对齐 contentRight。
constexpr int kFormRowStep = 34;

// 控件 ID
constexpr int kTabSectorsId = 2101;
constexpr int kTabProfilesId = 2102;
constexpr int kSectorCountComboId = 2107;
constexpr int kSectorListId = 2108;
constexpr int kActionTypeComboId = 2109;
constexpr int kActionNameEditId = 2110;
constexpr int kActionTargetEditId = 2111;
constexpr int kActionPickId = 2112;
constexpr int kActionModifierComboId = 2113;
constexpr int kActionKeyEditId = 2114;
constexpr int kActionStateHintId = 2115;
constexpr int kActionKeyHintId = 2116;
constexpr int kActionPlusId = 2117;  // 修饰键与主键之间的「+」
constexpr int kBlacklistListId = 2120;
constexpr int kBlacklistEditId = 2121;
constexpr int kBlacklistAddId = 2123;
constexpr int kBlacklistDeleteId = 2124;
constexpr int kBlacklistHintId = 2125;
constexpr int kOkId = 2130;
constexpr int kCancelId = 2131;

constexpr const wchar_t* kPropFont = L"KTileRadialFont";
constexpr const wchar_t* kPropTabFont = L"KTileRadialTabFont";

// 动作类型下拉的七项。第 0 项是「空」：空槽位在数据模型里是 isEmpty()
// 的动作，界面上必须能明确选到它，否则没法把配错的扇区清空。
struct ActionTypeOption {
    const char* key;   // 语言文件 key（文案不放表里）
    bool empty;
    core::RadialActionType type;
};

const ActionTypeOption kActionTypes[] = {
    {"radial.action.empty", true, core::RadialActionType::Application},
    {"radial.action.application", false, core::RadialActionType::Application},
    {"radial.action.file", false, core::RadialActionType::File},
    {"radial.action.folder", false, core::RadialActionType::Folder},
    {"radial.action.url", false, core::RadialActionType::Url},
    {"radial.action.shortcut", false, core::RadialActionType::Shortcut},
    {"radial.action.builtin", false, core::RadialActionType::Builtin},
};
constexpr int kActionTypeCount =
    static_cast<int>(sizeof(kActionTypes) / sizeof(kActionTypes[0]));

// 修饰键下拉（第 0 项 = 无）
struct ModifierOption {
    const char* key;
    bool none;
    core::HotkeyModifier modifier;
};

const ModifierOption kModifiers[] = {
    {"common.none", true, core::HotkeyModifier::Control},
    {"key.ctrl", false, core::HotkeyModifier::Control},
    {"key.shift", false, core::HotkeyModifier::Shift},
    {"key.alt", false, core::HotkeyModifier::Alt},
    {"key.win", false, core::HotkeyModifier::Win},
};
constexpr int kModifierCount =
    static_cast<int>(sizeof(kModifiers) / sizeof(kModifiers[0]));

// --- 状态

struct RadialEditorState {
    // 草稿：确定时才写回调用方
    core::RadialConfig draft;

    int activePage = 0;       // 0 = 扇区动作，1 = 按应用
    int currentSector = -1;   // 扇区列表选中行
    int currentProfile = -1;  // 按应用列表选中行

    // 程序化重建列表 / 回填表单时会触发通知，此时不写回草稿
    bool suppressNotify = false;

    std::vector<HWND> sectorPageControls;
    std::vector<HWND> profilePageControls;

    bool accepted = false;
};

// --- 小工具

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

std::wstring controlText(HWND dialog, const int id) {
    wchar_t buffer[1024] = {};
    HWND control = GetDlgItem(dialog, id);
    if (control == nullptr) {
        return std::wstring();
    }
    GetWindowTextW(control, buffer, 1024);
    return std::wstring(buffer);
}

void setControlText(HWND dialog, const int id, const std::wstring& text) {
    HWND control = GetDlgItem(dialog, id);
    if (control != nullptr) {
        SetWindowTextW(control, text.c_str());
    }
}

std::wstring trimWide(const std::wstring& text) {
    const std::size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// 扇区方位名：0° = 正上方，顺时针。表里只有八个方位，4/6/8 扇区就近取整：
// 用户看的是「我要往下拖」，八个方位够指路。
std::wstring sectorDirectionName(const std::uint8_t index,
                                 const std::uint8_t count) {
    static const char* kKeys[] = {
        "radial.sector.up",    "radial.sector.upRight",
        "radial.sector.right", "radial.sector.downRight",
        "radial.sector.down",  "radial.sector.downLeft",
        "radial.sector.left",  "radial.sector.upLeft"};
    if (count == 0) {
        return infrastructure::i18n::trw(kKeys[0]);
    }
    const float angle = 360.0F * static_cast<float>(index) /
                        static_cast<float>(count);
    int slot = static_cast<int>((angle + 22.5F) / 45.0F) % 8;
    if (slot < 0) {
        slot += 8;
    }
    return infrastructure::i18n::trw(kKeys[slot]);
}

const wchar_t* actionTypeLabel(const core::RadialAction& action) {
    if (action.isEmpty()) {
        return infrastructure::i18n::trw("radial.action.empty").c_str();
    }
    switch (action.type) {
        case core::RadialActionType::Application:
            return infrastructure::i18n::trw("radial.action.application").c_str();
        case core::RadialActionType::File:
            return infrastructure::i18n::trw("radial.action.file").c_str();
        case core::RadialActionType::Folder:
            return infrastructure::i18n::trw("radial.action.folder").c_str();
        case core::RadialActionType::Url:
            return infrastructure::i18n::trw("radial.action.url").c_str();
        case core::RadialActionType::Shortcut:
            return infrastructure::i18n::trw("radial.action.shortcut").c_str();
        case core::RadialActionType::Builtin:
            return infrastructure::i18n::trw("radial.action.builtin").c_str();
    }
    return infrastructure::i18n::trw("radial.action.application").c_str();
}

// 动作的「目标」列显示什么：快捷键显示组合键，其余显示 target
std::wstring actionTargetLabel(const core::RadialAction& action) {
    if (action.isEmpty()) {
        return std::wstring();
    }
    if (action.type == core::RadialActionType::Shortcut) {
        std::wstring text;
        for (const auto& modifier : action.modifiers) {
            switch (modifier) {
                case core::HotkeyModifier::Control: text += L"Ctrl+"; break;
                case core::HotkeyModifier::Shift:   text += L"Shift+"; break;
                case core::HotkeyModifier::Alt:     text += L"Alt+"; break;
                case core::HotkeyModifier::Win:     text += L"Win+"; break;
            }
        }
        text += base::utf8ToWide(action.key);
        return text;
    }
    if (action.type == core::RadialActionType::Builtin) {
        // 目标存的是 "ktile:<id>"，界面上显示本地化的名字更好认
        const std::wstring target = base::utf8ToWide(action.target);
        for (const auto& item : core::builtinItems()) {
            if (std::wstring(item.target) == target) {
                return builtinItemDisplayName(item);
            }
        }
        return target;
    }
    return base::utf8ToWide(action.target);
}

// 只有一份通用轮盘，包装留着是因为调用点很多：保留一处取值点，
// 容易看清哪份菜单在被编辑。
core::RadialMenu* currentMenu(RadialEditorState* state) {
    if (state == nullptr) {
        return nullptr;
    }
    return &state->draft.menu;
}

core::RadialAction* currentAction(RadialEditorState* state) {
    core::RadialMenu* menu = currentMenu(state);
    if (menu == nullptr || state->currentSector < 0 ||
        static_cast<std::size_t>(state->currentSector) >= menu->actions.size()) {
        return nullptr;
    }
    return &menu->actions[static_cast<std::size_t>(state->currentSector)];
}

// --- 列表刷新

void refreshSectorList(HWND dialog, RadialEditorState* state) {
    HWND list = GetDlgItem(dialog, kSectorListId);
    if (list == nullptr || state == nullptr) {
        return;
    }
    state->suppressNotify = true;
    ListView_DeleteAllItems(list);

    const core::RadialMenu* menu = currentMenu(state);
    if (menu != nullptr) {
        for (std::size_t i = 0; i < menu->actions.size(); ++i) {
            const core::RadialAction& action = menu->actions[i];
            const std::wstring sectorText =
                std::to_wstring(i + 1) + L" · " +
                sectorDirectionName(static_cast<std::uint8_t>(i),
                                    menu->sectorCount);

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<LPWSTR>(sectorText.c_str());
            item.lParam = static_cast<LPARAM>(i);
            const int index = ListView_InsertItem(list, &item);
            if (index < 0) {
                continue;
            }
            const std::wstring typeText = actionTypeLabel(action);
            const std::wstring nameText = base::utf8ToWide(radialActionName(action));
            const std::wstring targetText = actionTargetLabel(action);
            ListView_SetItemText(list, index, 1,
                                 const_cast<LPWSTR>(typeText.c_str()));
            ListView_SetItemText(list, index, 2,
                                 const_cast<LPWSTR>(nameText.c_str()));
            ListView_SetItemText(list, index, 3,
                                 const_cast<LPWSTR>(targetText.c_str()));
        }
    }

    if (menu != nullptr && state->currentSector >= 0 &&
        static_cast<std::size_t>(state->currentSector) < menu->actions.size()) {
        ListView_SetItemState(list, state->currentSector,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    state->suppressNotify = false;
}

// 只刷新当前行（边打字边更新列表，不重建、不丢选中）
void refreshSectorRow(HWND dialog, RadialEditorState* state) {
    HWND list = GetDlgItem(dialog, kSectorListId);
    core::RadialAction* action = currentAction(state);
    if (list == nullptr || action == nullptr) {
        return;
    }
    const int row = state->currentSector;
    const std::wstring typeText = actionTypeLabel(*action);
    const std::wstring nameText = base::utf8ToWide(radialActionName(*action));
    const std::wstring targetText = actionTargetLabel(*action);
    state->suppressNotify = true;
    ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(typeText.c_str()));
    ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(nameText.c_str()));
    ListView_SetItemText(list, row, 3, const_cast<LPWSTR>(targetText.c_str()));
    state->suppressNotify = false;
}

// 「类型」下拉当前选中的条目（0 = 「(空)」）。
//
// 表单可用性和动态提示都以它为准，不用动作数据的 isEmpty()：空扇区刚把
// 类型改成「快捷键」时，名称/目标/主键本来就全空，拿 isEmpty() 判断会把
// 输入框全部灰掉，这一格永远填不进去。类型下拉是唯一入口。
int selectedTypeIndex(HWND dialog) {
    HWND combo = GetDlgItem(dialog, kActionTypeComboId);
    if (combo == nullptr) {
        return 0;
    }
    const int selection =
        static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= kActionTypeCount) {
        return 0;
    }
    return selection;
}

void refreshActionStateHint(HWND dialog, RadialEditorState* state) {
    std::wstring text;
    const core::RadialMenu* menu = currentMenu(state);
    if (menu == nullptr) {
        text = infrastructure::i18n::trw("radialedit.hint.noProfile").c_str();
    } else if (state->currentSector < 0) {
        text = infrastructure::i18n::trw("radialedit.hint.selectSector").c_str();
    } else {
        // 提示也跟着「类型」下拉走：空扇区选好类型后立刻变成
        // 「这一格该填什么」。
        const ActionTypeOption& option = kActionTypes[selectedTypeIndex(dialog)];
        if (option.empty) {
            text = infrastructure::i18n::trw("radialedit.hint.emptySector").c_str();
        } else if (option.type == core::RadialActionType::Shortcut) {
            text = infrastructure::i18n::trw("radialedit.hint.shortcut").c_str();
        } else if (option.type == core::RadialActionType::Builtin) {
            text = infrastructure::i18n::trw("radialedit.hint.builtin").c_str();
        } else if (option.type == core::RadialActionType::Url) {
            text = infrastructure::i18n::trw("radialedit.hint.url").c_str();
        } else {
            text = infrastructure::i18n::trw("radialedit.hint.file").c_str();
        }
    }
    setControlText(dialog, kActionStateHintId, text);
}

// 表单可用性：只亮跟当前动作类型有关的部分。快捷键不需要「目标」（写回
// 时也会清掉），程序/文件/网址/内置命令不需要「修饰键 + 主键」；全灰只
// 留给「没选中扇区」。配合动态提示，一眼能看出这一格要填什么。
void refreshFormAvailability(HWND dialog, RadialEditorState* state) {
    // 能不能填看「类型」下拉（见 selectedTypeIndex），选中与否看数据。
    const bool hasSector = currentAction(state) != nullptr;
    const ActionTypeOption& option = kActionTypes[selectedTypeIndex(dialog)];
    const bool hasType = hasSector && !option.empty;
    const bool shortcut =
        hasType && option.type == core::RadialActionType::Shortcut;

    struct Entry {
        int id;
        bool enabled;
    };
    // 「类型」始终可用：空扇区得先能选类型
    const Entry entries[] = {
        {kActionTypeComboId, hasSector},
        {kActionNameEditId, hasType},
        {kActionTargetEditId, hasType && !shortcut},
        {kActionPickId, hasType && !shortcut},
        {kActionModifierComboId, shortcut},
        {kActionPlusId, shortcut},
        {kActionKeyEditId, shortcut},
        {kActionKeyHintId, shortcut},
    };
    for (const Entry& entry : entries) {
        HWND control = GetDlgItem(dialog, entry.id);
        if (control != nullptr) {
            EnableWindow(control, entry.enabled ? TRUE : FALSE);
        }
    }
}

// 把某个扇区的数据回填到动作表单
void refreshActionForm(HWND dialog, RadialEditorState* state) {
    if (state == nullptr) {
        return;
    }
    state->suppressNotify = true;

    HWND typeCombo = GetDlgItem(dialog, kActionTypeComboId);
    core::RadialAction* action = currentAction(state);

    int typeIndex = 0;
    if (action != nullptr) {
        if (action->isEmpty()) {
            typeIndex = 0;
        } else {
            for (int i = 1; i < kActionTypeCount; ++i) {
                if (kActionTypes[i].type == action->type) {
                    typeIndex = i;
                    break;
                }
            }
        }
    }
    if (typeCombo != nullptr) {
        SendMessageW(typeCombo, CB_SETCURSEL, static_cast<WPARAM>(typeIndex), 0);
    }

    setControlText(dialog, kActionNameEditId,
                   action != nullptr
                       ? base::utf8ToWide(radialActionName(*action))
                       : std::wstring());
    setControlText(dialog, kActionTargetEditId,
                   action != nullptr ? base::utf8ToWide(action->target)
                                     : std::wstring());
    setControlText(dialog, kActionKeyEditId,
                   action != nullptr ? base::utf8ToWide(action->key)
                                     : std::wstring());

    HWND modifierCombo = GetDlgItem(dialog, kActionModifierComboId);
    if (modifierCombo != nullptr) {
        int modifierIndex = 0;
        if (action != nullptr && !action->modifiers.empty()) {
            for (int i = 1; i < kModifierCount; ++i) {
                if (kModifiers[i].modifier == action->modifiers.front()) {
                    modifierIndex = i;
                    break;
                }
            }
        }
        SendMessageW(modifierCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(modifierIndex), 0);
    }

    // 可用性按「是否选中扇区 + 动作类型」决定，见 refreshFormAvailability
    state->suppressNotify = false;
    refreshFormAvailability(dialog, state);
    refreshActionStateHint(dialog, state);
}

// 黑名单列表：只列进程名，命中即「在这个应用里不弹轮盘」。
void refreshBlacklist(HWND dialog, RadialEditorState* state) {
    HWND list = GetDlgItem(dialog, kBlacklistListId);
    if (list == nullptr || state == nullptr) {
        return;
    }
    state->suppressNotify = true;
    ListView_DeleteAllItems(list);

    for (std::size_t i = 0; i < state->draft.blacklist.size(); ++i) {
        const std::wstring text = base::utf8ToWide(state->draft.blacklist[i]);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(text.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(list, &item);
    }

    if (state->currentProfile >= 0 &&
        static_cast<std::size_t>(state->currentProfile) <
            state->draft.blacklist.size()) {
        ListView_SetItemState(list, state->currentProfile,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    state->suppressNotify = false;
}

// 把选中的黑名单行回填到输入框（选中 → 改 → 添加即覆盖原项）
void refreshBlacklistForm(HWND dialog, RadialEditorState* state) {
    if (state == nullptr) {
        return;
    }
    state->suppressNotify = true;
    std::wstring process;
    if (state->currentProfile >= 0 &&
        static_cast<std::size_t>(state->currentProfile) <
            state->draft.blacklist.size()) {
        process = base::utf8ToWide(
            state->draft.blacklist[static_cast<std::size_t>(state->currentProfile)]);
    }
    setControlText(dialog, kBlacklistEditId, process);
    state->suppressNotify = false;
}

// --- 分页

void switchPage(HWND dialog, RadialEditorState* state, const int page) {
    if (state == nullptr) {
        return;
    }
    state->activePage = page;
    for (const HWND control : state->sectorPageControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (const HWND control : state->profilePageControls) {
        ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
    }
    // 两枚页签是自绘按钮，切页后要重画（选中态由 activePage 决定）
    InvalidateRect(GetDlgItem(dialog, kTabSectorsId), nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, kTabProfilesId), nullptr, TRUE);
    if (page == 1) {
        refreshBlacklist(dialog, state);
        refreshBlacklistForm(dialog, state);
    }
}

// --- 表单写回

// 把动作表单当前内容写回选中的扇区
void commitActionForm(HWND dialog, RadialEditorState* state) {
    core::RadialAction* action = currentAction(state);
    if (action == nullptr || state->suppressNotify) {
        return;
    }

    HWND typeCombo = GetDlgItem(dialog, kActionTypeComboId);
    int typeIndex = 0;
    if (typeCombo != nullptr) {
        const int selection = static_cast<int>(
            SendMessageW(typeCombo, CB_GETCURSEL, 0, 0));
        if (selection >= 0 && selection < kActionTypeCount) {
            typeIndex = selection;
        }
    }

    const std::wstring name = trimWide(controlText(dialog, kActionNameEditId));
    const std::wstring target =
        trimWide(controlText(dialog, kActionTargetEditId));
    const std::wstring key = trimWide(controlText(dialog, kActionKeyEditId));

    HWND modifierCombo = GetDlgItem(dialog, kActionModifierComboId);
    int modifierIndex = 0;
    if (modifierCombo != nullptr) {
        const int selection = static_cast<int>(
            SendMessageW(modifierCombo, CB_GETCURSEL, 0, 0));
        if (selection >= 0 && selection < kModifierCount) {
            modifierIndex = selection;
        }
    }

    if (kActionTypes[typeIndex].empty) {
        // 清空该扇区：所有字段一起重置，保证 isEmpty() 为真。
        *action = core::RadialAction();
        // 表单跟着清，否则列表已变回「(空)」而名称框还留着上一格的名字。
        state->suppressNotify = true;
        setControlText(dialog, kActionNameEditId, std::wstring());
        setControlText(dialog, kActionTargetEditId, std::wstring());
        setControlText(dialog, kActionKeyEditId, std::wstring());
        HWND modifierCombo = GetDlgItem(dialog, kActionModifierComboId);
        if (modifierCombo != nullptr) {
            SendMessageW(modifierCombo, CB_SETCURSEL, 0, 0);
        }
        state->suppressNotify = false;
        refreshSectorRow(dialog, state);
        refreshFormAvailability(dialog, state);
        refreshActionStateHint(dialog, state);
        return;
    }

    action->type = kActionTypes[typeIndex].type;
    // 名字没改就继续存 key，让它跟着语言切换；改过才固化用户输入，
    // 同时丢掉 key，否则翻译永远盖住他写的字。
    const std::string typedName = base::wideToUtf8Lossy(name);
    if (!action->nameKey.empty() && typedName == radialActionName(*action)) {
        action->name.clear();
    } else {
        action->name = typedName;
        action->nameKey.clear();
    }

    // 按类型清理无关字段：残留值会让动作「看着配好了、按了没反应」
    // （例如 target 已空而 key 还留着，isEmpty() 为假就去启动空路径）。
    // 数据与界面一起清，否则目标框还挂着上一类型的路径。
    const auto clearField = [&](const int id) {
        state->suppressNotify = true;
        setControlText(dialog, id, std::wstring());
        state->suppressNotify = false;
    };

    const auto type = action->type;
    if (type == core::RadialActionType::Shortcut) {
        action->target.clear();
        action->arguments.clear();
        action->modifiers.clear();
        if (!kModifiers[modifierIndex].none) {
            action->modifiers.push_back(kModifiers[modifierIndex].modifier);
        }
        action->key = base::wideToUtf8Lossy(key);
        clearField(kActionTargetEditId);
    } else if (type == core::RadialActionType::Builtin) {
        action->key.clear();
        action->modifiers.clear();
        // 允许用户直接粘贴 "ktile:lock"，也容忍只写 "lock"
        std::string value = base::wideToUtf8Lossy(target);
        if (!value.empty() && value.rfind("ktile:", 0) != 0) {
            value = "ktile:" + value;
        }
        action->target = value;
    } else {
        action->key.clear();
        action->modifiers.clear();
        action->target = base::wideToUtf8Lossy(target);
        clearField(kActionKeyEditId);
    }

    refreshSectorRow(dialog, state);
    refreshFormAvailability(dialog, state);
    refreshActionStateHint(dialog, state);
}

// --- 选择目标

// 内置命令选择：弹一个分组菜单，选中即写回 target
void pickBuiltinAction(HWND dialog, RadialEditorState* state) {
    core::RadialAction* action = currentAction(state);
    if (action == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    std::vector<std::wstring> targets;
    std::wstring currentCategory;
    int commandId = 1;
    for (const auto& item : core::builtinItems()) {
        // 分组判定用规范名，显示用译文
        const std::wstring category(item.category);
        if (category != currentCategory) {
            if (!currentCategory.empty()) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            const std::wstring shown = builtinCategoryDisplayName(item.category);
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, shown.c_str());
            currentCategory = category;
        }
        const std::wstring name = builtinItemDisplayName(item);
        AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(commandId),
                    name.c_str());
        targets.push_back(std::wstring(item.target));
        ++commandId;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const int chosen = static_cast<int>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                       cursor.x, cursor.y, 0, dialog, nullptr));
    DestroyMenu(menu);

    if (chosen <= 0 || static_cast<std::size_t>(chosen) > targets.size()) {
        return;
    }
    const std::wstring& target = targets[static_cast<std::size_t>(chosen) - 1];
    setControlText(dialog, kActionTargetEditId, target);

    // 名称留空时用系统项的名字，省得用户对着「内置命令」猜
    if (trimWide(controlText(dialog, kActionNameEditId)).empty()) {
        for (const auto& item : core::builtinItems()) {
            if (std::wstring(item.target) == target) {
                setControlText(dialog, kActionNameEditId,
                               builtinItemDisplayName(item));
                break;
            }
        }
    }
    commitActionForm(dialog, state);
}

// 文件 / 文件夹 / 程序的选择器
void pickTargetPath(HWND dialog, RadialEditorState* state) {
    core::RadialAction* action = currentAction(state);
    if (action == nullptr) {
        return;
    }

    HWND typeCombo = GetDlgItem(dialog, kActionTypeComboId);
    const int selection = typeCombo != nullptr
                              ? static_cast<int>(SendMessageW(
                                    typeCombo, CB_GETCURSEL, 0, 0))
                              : 0;
    const auto type = selection >= 0 && selection < kActionTypeCount
                          ? kActionTypes[selection].type
                          : core::RadialActionType::Application;

    if (type == core::RadialActionType::Builtin) {
        pickBuiltinAction(dialog, state);
        return;
    }
    if (type == core::RadialActionType::Shortcut ||
        type == core::RadialActionType::Url) {
        // 网址没有文件可挑；快捷键也不需要路径
        return;
    }

    if (type == core::RadialActionType::Folder) {
        BROWSEINFOW browse{};
        browse.hwndOwner = dialog;
        browse.lpszTitle = infrastructure::i18n::trw("radialedit.chooseFolderTitle").c_str();
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST idList = SHBrowseForFolderW(&browse);
        if (idList == nullptr) {
            return;
        }
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(idList, path) != FALSE) {
            setControlText(dialog, kActionTargetEditId, path);
            if (trimWide(controlText(dialog, kActionNameEditId)).empty()) {
                std::wstring name(path);
                const std::size_t slash = name.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    name = name.substr(slash + 1);
                }
                setControlText(dialog, kActionNameEditId, name);
            }
            commitActionForm(dialog, state);
        }
        ILFree(idList);
        return;
    }

    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialog;
    // 过滤器用 NUL 分隔，只能运行时拼；文案来自语言文件。
    const std::wstring programFilter =
        infrastructure::i18n::trw("filter.program") + L'\0' +
        L"*.exe;*.lnk;*.bat" + L'\0' +
        infrastructure::i18n::trw("filter.allFiles") + L'\0' +
        L"*.*" + L'\0';
    ofn.lpstrFilter = programFilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn) == FALSE) {
        return;
    }
    setControlText(dialog, kActionTargetEditId, file);
    if (trimWide(controlText(dialog, kActionNameEditId)).empty()) {
        std::wstring name(file);
        const std::size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            name = name.substr(slash + 1);
        }
        const std::size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            name = name.substr(0, dot);
        }
        setControlText(dialog, kActionNameEditId, name);
    }
    commitActionForm(dialog, state);
}

// --- 窗口过程

LRESULT CALLBACK editorProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam);

void ensureEditorClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &editorProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kEditorClass;
    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

void createControls(HWND hwnd, RadialEditorState* state) {
    HFONT font = dialogFont();
    SetPropW(hwnd, kPropFont, font);

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

    std::vector<HWND>* sink = nullptr;

    auto makeControl = [&](const wchar_t* className, const wchar_t* text,
                           const DWORD style, const DWORD exStyle, const int x,
                           const int y, const int w, const int h,
                           const int id) -> HWND {
        HWND control = CreateWindowExW(
            exStyle, className, text, style, scaleFor(hwnd, x),
            scaleFor(hwnd, y), scaleFor(hwnd, w), scaleFor(hwnd, h), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
            nullptr);
        if (control != nullptr && font != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                         TRUE);
        }
        if (control != nullptr && sink != nullptr) {
            sink->push_back(control);
        }
        return control;
    };

    auto makeLabel = [&](const wchar_t* text, const int x, const int y,
                         const int w, const int h) -> HWND {
        return makeControl(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                           x, y, w, h, -1);
    };

    // 取词助手：文案都在语言文件里，这里只做取值与指针转换
    auto T = [](const char* key) -> const wchar_t* {
        return infrastructure::i18n::trw(key).c_str();
    };

    // 用对话框字体现量文字，按 96 DPI 逻辑像素返回。
    // 不能按中文字数手工排像素：英文的「Modifier」「Choose…」会被裁字。
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
        return MulDiv(size.cx, 96, dpi);
    };
    // 按钮 / 下拉：文字宽 + 内边距与箭头占位
    auto fitWidth = [&](const wchar_t* text, const int minWidth) -> int {
        return (std::max)(minWidth, measureText(text) + 22);
    };

    // --- 顶部两枚页签（自绘：选中页画成强调色主按钮）
    sink = nullptr;
    const int tabSectorsW = fitWidth(T("radialedit.tab.sectors"), 96);
    const int tabBlacklistX = kTabLeft + tabSectorsW + 8;
    makeControl(L"BUTTON", T("radialedit.tab.sectors"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
                0, kTabLeft, 14, tabSectorsW, kButtonHeight, kTabSectorsId);
    makeControl(L"BUTTON", T("radialedit.tab.blacklist"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                tabBlacklistX, 14, fitWidth(T("radialedit.tab.blacklist"), 96),
                kButtonHeight, kTabProfilesId);

    // --- 第 1 页：扇区动作
    sink = &state->sectorPageControls;

    // 扇区数：[4 扇区 ▾] 跟在标签后面，标签宽度按实测递推
    const int countLabelW = measureText(T("radialedit.sectorCount")) + 18;
    makeLabel(T("radialedit.sectorCount"), kRightLeft, 52, countLabelW, 18);
    const int countComboX = kRightLeft + countLabelW;
    HWND countCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        countComboX, 48, fitWidth(T("radialedit.sectors.4"), 90), 160,
        kSectorCountComboId);
    if (countCombo != nullptr) {
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(T("radialedit.sectors.4")));
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(T("radialedit.sectors.6")));
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(T("radialedit.sectors.8")));
        SendMessageW(countCombo, CB_SETCURSEL, 2, 0);
    }

    // --- 扇区列表
    //
    // 四列宽度按内容实测/估宽排：类型列按最长的动作类型名（英文
    // Application 比中文「快捷键」宽一倍），目标列吃满剩余。
    HWND sectorList = makeControl(
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        WS_EX_CLIENTEDGE, kRightLeft, kSectorListTop, kRightWidth,
        kSectorListHeight, kSectorListId);
    if (sectorList != nullptr) {
        ListView_SetExtendedListViewStyle(
            sectorList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        int sectorColumnW =
            (std::max)(92, measureText(T("radialedit.col.sector")) + 24);
        int typeColumnW =
            (std::max)(88, measureText(T("radialedit.col.type")) + 24);
        for (int i = 0; i < kActionTypeCount; ++i) {
            typeColumnW =
                (std::max)(typeColumnW, measureText(T(kActionTypes[i].key)) + 16);
        }
        const int nameColumnW = 150;
        const int targetColumnW =
            kRightWidth - 8 - sectorColumnW - typeColumnW - nameColumnW;

        struct ColumnSpec {
            const wchar_t* text;
            int width;
        };
        const ColumnSpec columns[] = {
            {T("radialedit.col.sector"), sectorColumnW},
            {T("radialedit.col.type"), typeColumnW},
            {T("radialedit.col.name"), nameColumnW},
            {T("radialedit.col.target"), targetColumnW},
        };
        const int columnCount =
            static_cast<int>(sizeof(columns) / sizeof(columns[0]));
        for (int i = 0; i < columnCount; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<LPWSTR>(columns[i].text);
            column.cx = scaleFor(hwnd, columns[i].width);
            column.fmt = LVCFMT_LEFT;
            ListView_InsertColumn(sectorList, i, &column);
        }
    }

    // --- 动作表单
    //
    // 三行网格 + 一行提示：标签列统一实测宽度、控件列共用左边界 fieldX、
    // 右边缘对齐 contentRight；「修饰键 + 主键」用「+」连成一串，与列表
    // 「目标」列是同一种读法。
    const int formTop = kSectorListTop + kSectorListHeight + 16;  // 288
    const int contentRight = kDialogWidth - kMargin;              // 684

    int formLabelW = measureText(T("radialedit.col.type"));
    formLabelW = (std::max)(formLabelW, measureText(T("radialedit.col.name")));
    formLabelW = (std::max)(formLabelW, measureText(T("radialedit.col.target")));
    formLabelW = (std::max)(formLabelW, measureText(T("radialedit.modifier")));
    formLabelW = (std::max)(formLabelW + 16, 52);
    const int fieldX = kRightLeft + formLabelW;

    // 第 1 行：类型 + 名称
    const int row1 = formTop;
    makeLabel(T("radialedit.col.type"), kRightLeft, row1 + 4, formLabelW - 8, 18);
    int typeComboW = 100;
    for (int i = 0; i < kActionTypeCount; ++i) {
        typeComboW =
            (std::max)(typeComboW, measureText(T(kActionTypes[i].key)) + 30);
    }
    HWND typeCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        fieldX, row1, typeComboW, 220, kActionTypeComboId);
    if (typeCombo != nullptr) {
        for (int i = 0; i < kActionTypeCount; ++i) {
            SendMessageW(typeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(T(kActionTypes[i].key)));
        }
        SendMessageW(typeCombo, CB_SETCURSEL, 0, 0);
    }

    const int nameLabelX = fieldX + typeComboW + 24;
    const int nameLabelW = measureText(T("radialedit.col.name")) + 8;
    makeLabel(T("radialedit.col.name"), nameLabelX, row1 + 4, nameLabelW, 18);
    const int nameEditX = nameLabelX + nameLabelW + 8;
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, nameEditX, row1 - 2,
                contentRight - nameEditX, 24, kActionNameEditId);

    // 第 2 行：目标（路径 / 网址 / 内置命令），「选择…」贴右边界
    const int row2 = formTop + kFormRowStep;
    makeLabel(T("radialedit.col.target"), kRightLeft, row2 + 4, formLabelW - 8, 18);
    const int chooseW = fitWidth(T("radialedit.choose"), 64);
    const int chooseX = contentRight - chooseW;
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, fieldX, row2 - 2, chooseX - 8 - fieldX, 24,
                kActionTargetEditId);
    makeControl(L"BUTTON", T("radialedit.choose"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                chooseX, row2 - 2, chooseW, kSmallButtonHeight, kActionPickId);

    // 第 3 行：修饰键 + 主键（「+」把它们连成一个组合键）
    const int row3 = formTop + kFormRowStep * 2;
    makeLabel(T("radialedit.modifier"), kRightLeft, row3 + 4, formLabelW - 8, 18);
    int modifierW = 90;
    for (int i = 0; i < kModifierCount; ++i) {
        modifierW =
            (std::max)(modifierW, measureText(T(kModifiers[i].key)) + 30);
    }
    HWND modifierCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        fieldX, row3, modifierW, 160, kActionModifierComboId);
    if (modifierCombo != nullptr) {
        for (int i = 0; i < kModifierCount; ++i) {
            SendMessageW(modifierCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(T(kModifiers[i].key)));
        }
        SendMessageW(modifierCombo, CB_SETCURSEL, 0, 0);
    }

    // 「+」左右各留 6：太贴会看成修饰键下拉的一部分
    const int plusX = fieldX + modifierW + 6;
    makeControl(L"STATIC", L"+", WS_CHILD | WS_VISIBLE | SS_CENTER,
                0, plusX, row3 + 4, 14, 18, kActionPlusId);
    const int keyX = plusX + 20;
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, keyX, row3 - 2, 64, 24, kActionKeyEditId);
    const int keyHintX = keyX + 64 + 12;
    makeControl(L"STATIC", T("radialedit.keyHint"),
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, keyHintX, row3 + 4,
                contentRight - keyHintX, 18, kActionKeyHintId);

    // 第 4 行：动态提示（两行高度），在卡片下沿之前收住，不压底部按钮。
    makeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                kRightLeft, row3 + kFormRowStep, kRightWidth, 38,
                kActionStateHintId);

    // --- 第 2 页：黑名单
    sink = &state->profilePageControls;

    // 这一页没有「扇区数」行，列表从页签下方起、一直铺到表单上沿，
    // 两页的动作区落在同一条水平线上。
    const int blacklistListTop = 48;
    HWND profileList = makeControl(
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        WS_EX_CLIENTEDGE, kMargin, blacklistListTop,
        kDialogWidth - 2 * kMargin,
        kSectorListTop + kSectorListHeight - blacklistListTop,
        kBlacklistListId);
    if (profileList != nullptr) {
        ListView_SetExtendedListViewStyle(
            profileList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<LPWSTR>(T("radialedit.blacklistColumn"));
        column.cx = scaleFor(hwnd, kDialogWidth - 2 * kMargin - 8);
        column.fmt = LVCFMT_LEFT;
        ListView_InsertColumn(profileList, 0, &column);
    }

    // 进程名一行：输入框吃满到按钮之前，两个按钮贴右边界、按实测宽度
    const int profileFormTop = blacklistListTop +
                               (kSectorListTop + kSectorListHeight - blacklistListTop) + 16;
    const int processLabelW = measureText(T("radialedit.processName")) + 12;
    makeLabel(T("radialedit.processName"), kMargin, profileFormTop + 4,
              processLabelW, 18);
    const int removeW = fitWidth(T("radialedit.removeSelected"), 96);
    const int addW = fitWidth(T("radialedit.addOrUpdate"), 96);
    const int removeX = kDialogWidth - kMargin - removeW;
    const int addX = removeX - 8 - addW;
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, kMargin + processLabelW,
                profileFormTop - 2, addX - 8 - (kMargin + processLabelW), 24,
                kBlacklistEditId);
    makeControl(L"BUTTON", T("radialedit.addOrUpdate"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                addX, profileFormTop - 2, addW, kSmallButtonHeight,
                kBlacklistAddId);
    makeControl(L"BUTTON", T("radialedit.removeSelected"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                removeX, profileFormTop - 2, removeW, kSmallButtonHeight,
                kBlacklistDeleteId);

    // 说明按四行给高度：150% 缩放下这段中文会折到第四行，给少了末尾会被裁掉
    makeControl(
        L"STATIC", T("radialedit.blacklistHint"),
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kMargin, profileFormTop + 34,
        kDialogWidth - 2 * kMargin, 88, kBlacklistHintId);

    // ---- 底部按钮 ----
    sink = nullptr;
    const int buttonY = kDialogHeight - kMargin - kButtonHeight;
    makeControl(L"BUTTON", infrastructure::i18n::trw("common.ok").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kDialogWidth - kMargin - 2 * kButtonWidth - 8, buttonY,
                kButtonWidth, kButtonHeight, kOkId);
    makeControl(L"BUTTON", infrastructure::i18n::trw("common.cancel").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kDialogWidth - kMargin - kButtonWidth, buttonY, kButtonWidth,
                kButtonHeight, kCancelId);

    // 第 2 页的控件先藏起来
    for (const HWND control : state->profilePageControls) {
        ShowWindow(control, SW_HIDE);
    }
}

LRESULT CALLBACK editorProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    auto* state = reinterpret_cast<RadialEditorState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            dialog_style::paintBackground(reinterpret_cast<HDC>(wParam), client);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc != nullptr) {
                RECT card{};
                card.left = scaleFor(hwnd, 8);
                card.top = scaleFor(hwnd, 8);
                card.right = scaleFor(hwnd, kDialogWidth - 8);
                // 卡片下沿停在按钮行之上，与设置窗口的视觉节奏一致
                card.bottom = scaleFor(hwnd, kDialogHeight - kMargin -
                                                 kButtonHeight - 12);
                dialog_style::paintCard(dc, card, scaleFor(hwnd, 8));
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // 说明文字不铺灰底，直接画在白色卡片上；提示用次要色区分，
            // 禁用的控件仍用系统灰字，否则看起来还像能输入。
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            const int id = control != nullptr ? GetDlgCtrlID(control) : 0;
            COLORREF color = dialog_style::text();
            if (control != nullptr && IsWindowEnabled(control) == FALSE) {
                color = GetSysColor(COLOR_GRAYTEXT);
            } else if (id == kActionStateHintId || id == kActionKeyHintId ||
                       id == kBlacklistHintId) {
                color = dialog_style::secondaryText();
            }
            SetTextColor(dc, color);
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }
            createControls(hwnd, state);

            // 载入草稿到界面
            {
                const core::RadialMenu& menu = state->draft.menu;
                HWND countCombo = GetDlgItem(hwnd, kSectorCountComboId);
                if (countCombo != nullptr) {
                    const int index = menu.sectorCount >= 8
                                          ? 2
                                          : (menu.sectorCount >= 6 ? 1 : 0);
                    SendMessageW(countCombo, CB_SETCURSEL,
                                 static_cast<WPARAM>(index), 0);
                }
            }
            refreshSectorList(hwnd, state);
            refreshActionForm(hwnd, state);
            refreshBlacklist(hwnd, state);
            refreshBlacklistForm(hwnd, state);
            switchPage(hwnd, state, 0);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr || draw->CtlType != ODT_BUTTON ||
                state == nullptr) {
                break;
            }
            wchar_t text[64] = {};
            GetWindowTextW(draw->hwndItem, text, 64);

            const bool isTab = draw->CtlID == static_cast<UINT>(kTabSectorsId) ||
                               draw->CtlID == static_cast<UINT>(kTabProfilesId);
            bool primary = draw->CtlID == static_cast<UINT>(kOkId);
            if (isTab) {
                const int tabPage =
                    draw->CtlID == static_cast<UINT>(kTabSectorsId) ? 0 : 1;
                primary = state->activePage == tabPage;
            }
            dialog_style::paintButton(
                draw->hDC, draw->rcItem, text,
                static_cast<HFONT>(GetPropW(hwnd, kPropFont)),
                primary ? dialog_style::ButtonKind::Primary
                        : dialog_style::ButtonKind::Secondary,
                (draw->itemState & ODS_SELECTED) != 0,
                (draw->itemState & ODS_DISABLED) != 0,
                (draw->itemState & ODS_FOCUS) != 0 &&
                    (draw->itemState & ODS_NOFOCUSRECT) == 0);
            return TRUE;
        }

        case WM_NOTIFY: {
            if (state == nullptr) {
                break;
            }
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header == nullptr) {
                break;
            }
            if (header->idFrom == static_cast<UINT_PTR>(kSectorListId) &&
                header->code == LVN_ITEMCHANGED && !state->suppressNotify) {
                HWND list = GetDlgItem(hwnd, kSectorListId);
                const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                state->currentSector = selected;
                refreshActionForm(hwnd, state);
                return 0;
            }
            if (header->idFrom == static_cast<UINT_PTR>(kBlacklistListId) &&
                header->code == LVN_ITEMCHANGED && !state->suppressNotify) {
                HWND list = GetDlgItem(hwnd, kBlacklistListId);
                const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                state->currentProfile = selected;
                refreshBlacklistForm(hwnd, state);
                return 0;
            }
            break;
        }

        case WM_COMMAND: {
            int controlId = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (state == nullptr) {
                break;
            }

            // 回车 / Esc 由 IsDialogMessage 发 IDOK / IDCANCEL
            if (controlId == IDOK) {
                controlId = kOkId;
            } else if (controlId == IDCANCEL) {
                controlId = kCancelId;
            }

            if ((controlId == kTabSectorsId || controlId == kTabProfilesId) &&
                notification == BN_CLICKED) {
                switchPage(hwnd, state, controlId == kTabSectorsId ? 0 : 1);
                return 0;
            }

            if (controlId == kOkId) {
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (controlId == kCancelId) {
                state->accepted = false;
                DestroyWindow(hwnd);
                return 0;
            }

            if (controlId == kSectorCountComboId &&
                notification == CBN_SELCHANGE) {
                core::RadialMenu* menu = currentMenu(state);
                HWND combo = GetDlgItem(hwnd, kSectorCountComboId);
                if (menu != nullptr && combo != nullptr) {
                    const int selection = static_cast<int>(
                        SendMessageW(combo, CB_GETCURSEL, 0, 0));
                    const std::uint8_t count =
                        selection == 0 ? 4 : (selection == 1 ? 6 : 8);
                    if (count != menu->sectorCount) {
                        menu->sectorCount = count;
                        // 不变量：actions.size() == sectorCount。
                        // resize 保留原有扇区，缩小时直接裁掉多余的。
                        menu->actions.resize(count);
                        state->currentSector = -1;
                        refreshSectorList(hwnd, state);
                        refreshActionForm(hwnd, state);
                    }
                }
                return 0;
            }

            // ---- 动作表单：任一改动都写回当前扇区 ----
            if (!state->suppressNotify &&
                (controlId == kActionTypeComboId ||
                 controlId == kActionNameEditId ||
                 controlId == kActionTargetEditId ||
                 controlId == kActionModifierComboId ||
                 controlId == kActionKeyEditId)) {
                const bool interesting =
                    (controlId == kActionTypeComboId &&
                     notification == CBN_SELCHANGE) ||
                    (controlId == kActionModifierComboId &&
                     notification == CBN_SELCHANGE) ||
                    ((controlId == kActionNameEditId ||
                      controlId == kActionTargetEditId ||
                      controlId == kActionKeyEditId) &&
                     notification == EN_CHANGE);
                if (interesting) {
                    commitActionForm(hwnd, state);
                }
                return 0;
            }

            if (controlId == kActionPickId && notification == BN_CLICKED) {
                pickTargetPath(hwnd, state);
                return 0;
            }

            // ---- 黑名单页 ----
            if (controlId == kBlacklistAddId && notification == BN_CLICKED) {
                const std::wstring process =
                    trimWide(controlText(hwnd, kBlacklistEditId));
                if (process.empty()) {
                    MessageBoxW(hwnd, infrastructure::i18n::trw("radialedit.needProcess").c_str(),
                                infrastructure::i18n::trw("radialedit.tab.blacklist").c_str(), MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                const std::string pattern = base::wideToUtf8Lossy(process);

                const bool editing =
                    state->currentProfile >= 0 &&
                    static_cast<std::size_t>(state->currentProfile) <
                        state->draft.blacklist.size();
                const std::size_t slot =
                    editing ? static_cast<std::size_t>(state->currentProfile)
                            : state->draft.blacklist.size();

                // 判重时跳过选中行自身：「把 A 改成 A」不该报重复
                for (std::size_t i = 0; i < state->draft.blacklist.size(); ++i) {
                    if (editing && i == slot) continue;
                    if (core::processNameEquals(state->draft.blacklist[i],
                                                pattern)) {
                        MessageBoxW(hwnd, infrastructure::i18n::trw("radialedit.duplicateProcess").c_str(),
                                    infrastructure::i18n::trw("radialedit.tab.blacklist").c_str(), MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                }

                if (editing) {
                    state->draft.blacklist[slot] = pattern;
                } else {
                    state->draft.blacklist.push_back(pattern);
                    state->currentProfile =
                        static_cast<int>(state->draft.blacklist.size()) - 1;
                }
                refreshBlacklist(hwnd, state);
                refreshBlacklistForm(hwnd, state);
                return 0;
            }

            if (controlId == kBlacklistDeleteId && notification == BN_CLICKED) {
                if (state->currentProfile < 0 ||
                    static_cast<std::size_t>(state->currentProfile) >=
                        state->draft.blacklist.size()) {
                    MessageBoxW(hwnd, infrastructure::i18n::trw("radialedit.selectToRemove").c_str(),
                                infrastructure::i18n::trw("radialedit.tab.blacklist").c_str(), MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                state->draft.blacklist.erase(
                    state->draft.blacklist.begin() + state->currentProfile);
                state->currentProfile = -1;
                refreshBlacklist(hwnd, state);
                refreshBlacklistForm(hwnd, state);
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            HFONT font = static_cast<HFONT>(RemovePropW(hwnd, kPropFont));
            HFONT tabFont = static_cast<HFONT>(RemovePropW(hwnd, kPropTabFont));
            if (font != nullptr) {
                DeleteObject(font);
            }
            if (tabFont != nullptr) {
                DeleteObject(tabFont);
            }
            PostMessageW(hwnd, kEditorCloseMessage, 0, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

// --- 对外入口

bool showRadialEditorWindow(HWND owner, core::RadialConfig* config) {
    if (config == nullptr) {
        return false;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(INITCOMMONCONTROLSEX);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    HINSTANCE instance = nullptr;
    if (owner != nullptr) {
        instance = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    }
    if (instance == nullptr) {
        instance = GetModuleHandleW(nullptr);
    }
    ensureEditorClass(instance);

    RadialEditorState state;
    state.draft = *config;
    // 兜底：动作数量和扇区数对不上时补成 8 扇区骨架（首启、配置被清空、
    // 手改文件不一致等）。编辑器要求两者恒等。
    if (state.draft.menu.actions.size() != state.draft.menu.sectorCount ||
        state.draft.menu.sectorCount < core::kMinSectors) {
        state.draft.menu.sectorCount = 8;
        state.draft.menu.actions.assign(8, core::RadialAction());
    }
    // 打开就选中第 1 个扇区：否则列表空选、表单整片灰着，用户得先猜
    // 要先点列表那一行。
    state.currentSector = 0;

    RECT desired{0, 0, scaleFor(owner, kDialogWidth),
                 scaleFor(owner, kDialogHeight)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    HWND dialog = CreateWindowExW(exStyle, kEditorClass, infrastructure::i18n::trw("radialedit.title").c_str(),
                                  style, 0, 0, desired.right - desired.left,
                                  desired.bottom - desired.top, owner, nullptr,
                                  instance, &state);
    if (dialog == nullptr) {
        return false;
    }

    // 居中于 owner（设置窗口），owner 为空则居中于工作区
    RECT anchor{};
    if (owner != nullptr && GetWindowRect(owner, &anchor) != FALSE) {
        const int width = desired.right - desired.left;
        const int height = desired.bottom - desired.top;
        const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
        const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
        SetWindowPos(dialog, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = desired.right - desired.left;
        const int height = desired.bottom - desired.top;
        SetWindowPos(dialog, nullptr, work.left + ((work.right - work.left) - width) / 2,
                     work.top + ((work.bottom - work.top) - height) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环（GetMessage + IsDialogMessage，IsWindow 作退出条件，
    // 绝不发 WM_QUIT）
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kEditorCloseMessage) {
            continue;
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
    *config = state.draft;
    return true;
}

}  // namespace ui
}  // namespace ktile
