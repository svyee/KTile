// KTile - UI 层：轮盘菜单编辑器（实现）
//
// 界面分两页（左上角两枚按钮切换）：
//   扇区动作 —— 左列轮盘方案列表，右侧该方案的扇区列表 + 选中扇区的动作表单；
//   按应用   —— 进程名 → 方案 的映射表，匹配不到时回退全局默认。
//
#include "ui/radial_editor_dialog.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "ui/dialog_style.h"

#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// ---------------------------------------------------------------- 常量

constexpr const wchar_t* kEditorClass = L"KTile.RadialEditor";

// 对话框关闭通知（唤醒本对话框的嵌套模态循环，语义与设置窗口一致）
constexpr UINT kEditorCloseMessage = WM_APP + 0x91U;

constexpr int kDialogWidth = 700;
// 高度按最密的一页（扇区动作：列表 + 三行动作表单）定，两页共用。
// 再高就是纯留白 —— 实测 560 时底部空了近 90 DIP。
constexpr int kDialogHeight = 460;
constexpr int kMargin = 16;
constexpr int kButtonWidth = 78;
constexpr int kButtonHeight = 26;
constexpr int kSmallButtonHeight = 24;

// 左列（方案列表）
constexpr int kTabLeft = 16;      // 顶部两枚页签的起始横坐标

// 右列（扇区列表 + 动作表单）
// 左侧原来放「轮盘方案」列表（多方案时代的产物）。现在只有一份通用轮盘，
// 那 168px 还给内容区，扇区列表和动作表单都能更宽。
constexpr int kRightLeft = kMargin;
constexpr int kRightWidth = kDialogWidth - kMargin * 2;  // 668
constexpr int kSectorListTop = 74;
constexpr int kSectorListHeight = 210;

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
constexpr int kBlacklistListId = 2120;
constexpr int kBlacklistEditId = 2121;
constexpr int kBlacklistAddId = 2123;
constexpr int kBlacklistDeleteId = 2124;
constexpr int kBlacklistHintId = 2125;
constexpr int kOkId = 2130;
constexpr int kCancelId = 2131;

constexpr const wchar_t* kPropFont = L"KTileRadialFont";
constexpr const wchar_t* kPropTabFont = L"KTileRadialTabFont";

// 动作类型下拉的七项。第 0 项是「空」——空槽位在数据模型里是一个
// isEmpty() 的动作，界面上必须能明确选到它，否则用户没法把配错的扇区清空。
struct ActionTypeOption {
    const wchar_t* label;
    bool empty;
    core::RadialActionType type;
};

const ActionTypeOption kActionTypes[] = {
    {L"（空）", true, core::RadialActionType::Application},
    {L"程序", false, core::RadialActionType::Application},
    {L"文件", false, core::RadialActionType::File},
    {L"文件夹", false, core::RadialActionType::Folder},
    {L"网址", false, core::RadialActionType::Url},
    {L"快捷键", false, core::RadialActionType::Shortcut},
    {L"内置命令", false, core::RadialActionType::Builtin},
};
constexpr int kActionTypeCount =
    static_cast<int>(sizeof(kActionTypes) / sizeof(kActionTypes[0]));

// 修饰键下拉（第 0 项 = 无）
struct ModifierOption {
    const wchar_t* label;
    bool none;
    core::HotkeyModifier modifier;
};

const ModifierOption kModifiers[] = {
    {L"无", true, core::HotkeyModifier::Control},
    {L"Ctrl", false, core::HotkeyModifier::Control},
    {L"Shift", false, core::HotkeyModifier::Shift},
    {L"Alt", false, core::HotkeyModifier::Alt},
    {L"Win", false, core::HotkeyModifier::Win},
};
constexpr int kModifierCount =
    static_cast<int>(sizeof(kModifiers) / sizeof(kModifiers[0]));

// ---------------------------------------------------------------- 状态

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

// ---------------------------------------------------------------- 小工具

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

// 扇区方位名：0° = 正上方，顺时针。
//
// 表里只有八个方位，4/6/8 扇区都就近取整 —— 用户看的是「我要往下拖」，
// 精确角度对操作没有帮助，八个方位已经够指路。
const wchar_t* sectorDirectionName(const std::uint8_t index,
                                   const std::uint8_t count) {
    static const wchar_t* kNames[] = {L"上", L"右上", L"右", L"右下",
                                      L"下", L"左下", L"左", L"左上"};
    if (count == 0) {
        return kNames[0];
    }
    const float angle = 360.0F * static_cast<float>(index) /
                        static_cast<float>(count);
    int slot = static_cast<int>((angle + 22.5F) / 45.0F) % 8;
    if (slot < 0) {
        slot += 8;
    }
    return kNames[slot];
}

const wchar_t* actionTypeLabel(const core::RadialAction& action) {
    if (action.isEmpty()) {
        return L"（空）";
    }
    switch (action.type) {
        case core::RadialActionType::Application: return L"程序";
        case core::RadialActionType::File:        return L"文件";
        case core::RadialActionType::Folder:      return L"文件夹";
        case core::RadialActionType::Url:         return L"网址";
        case core::RadialActionType::Shortcut:    return L"快捷键";
        case core::RadialActionType::Builtin:     return L"内置命令";
    }
    return L"程序";
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
        // 目标存的是 "ktile:<id>"，界面上只显示中文名更好认
        const std::wstring target = base::utf8ToWide(action.target);
        for (const auto& item : core::builtinItems()) {
            if (std::wstring(item.target) == target) {
                return std::wstring(item.name);
            }
        }
        return target;
    }
    return base::utf8ToWide(action.target);
}

// 现在只有一份通用轮盘，这个包装留着是因为调用点很多 —— 都改成
// &state->draft.menu 也行，但保留一处取值点更容易看清「哪份菜单在被编辑」。
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

// ---------------------------------------------------------------- 列表刷新

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
            const std::wstring nameText = base::utf8ToWide(action.name);
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
    const std::wstring nameText = base::utf8ToWide(action->name);
    const std::wstring targetText = actionTargetLabel(*action);
    state->suppressNotify = true;
    ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(typeText.c_str()));
    ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(nameText.c_str()));
    ListView_SetItemText(list, row, 3, const_cast<LPWSTR>(targetText.c_str()));
    state->suppressNotify = false;
}

void refreshActionStateHint(HWND dialog, RadialEditorState* state) {
    std::wstring text;
    const core::RadialMenu* menu = currentMenu(state);
    if (menu == nullptr) {
        text = L"先在左侧新建一个轮盘方案。";
    } else if (state->currentSector < 0) {
        text = L"选中上方扇区后即可编辑它的动作。";
    } else if (const core::RadialAction* action = currentAction(state)) {
        if (action->isEmpty()) {
            text = L"该扇区为空，拖动到这里不会触发任何动作。";
        } else if (action->type == core::RadialActionType::Shortcut) {
            text = L"快捷键动作会注入到当前前台窗口，例如复制 = Ctrl + C。";
        } else if (action->type == core::RadialActionType::Builtin) {
            text = L"内置命令：点「选择…」从系统项列表里挑，不用手写 id。";
        } else if (action->type == core::RadialActionType::Url) {
            text = L"网址动作会交给默认浏览器打开。";
        } else {
            text = L"「选择…」挑文件或文件夹；也可以直接粘贴路径。";
        }
    }
    setControlText(dialog, kActionStateHintId, text);
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
                   action != nullptr ? base::utf8ToWide(action->name)
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

    // 表单的可用性跟着「是否选中扇区」走，未选中时灰掉比让人白点强
    const bool enabled = action != nullptr;
    const int ids[] = {kActionTypeComboId, kActionNameEditId,
                       kActionTargetEditId, kActionPickId,
                       kActionModifierComboId, kActionKeyEditId};
    for (const int id : ids) {
        HWND control = GetDlgItem(dialog, id);
        if (control != nullptr) {
            EnableWindow(control, enabled ? TRUE : FALSE);
        }
    }

    state->suppressNotify = false;
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

// ---------------------------------------------------------------- 分页

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

// ---------------------------------------------------------------- 表单写回

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
        // 清空该扇区：所有字段一起重置，保证 isEmpty() 为真
        *action = core::RadialAction();
        refreshSectorRow(dialog, state);
        refreshActionStateHint(dialog, state);
        return;
    }

    action->type = kActionTypes[typeIndex].type;
    action->name = base::wideToUtf8Lossy(name);

    // 按类型清理无关字段：留着上一类型的残留值会让动作「看起来配好了、
    // 实际一按就什么都不发生」（例如切换成「程序」但 target 还是空的，
    // 而 key 还留着 C —— isEmpty() 为假，于是去启动一个空路径）。
    // 数据与界面一起清，否则用户看到「目标」里还挂着上一个类型的路径。
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
    refreshActionStateHint(dialog, state);
}

// ---------------------------------------------------------------- 选择目标

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
        const std::wstring category(item.category);
        if (category != currentCategory) {
            if (!currentCategory.empty()) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, category.c_str());
            currentCategory = category;
        }
        const std::wstring name(item.name);
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
                setControlText(dialog, kActionNameEditId, item.name);
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
        browse.lpszTitle = L"选择文件夹";
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
    ofn.lpstrFilter =
        L"程序 (*.exe;*.lnk;*.bat)\\0*.exe;*.lnk;*.bat\\0所有文件\\0*.*\\0";
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

// ---------------------------------------------------------------- 窗口过程

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

    // ---- 顶部两枚页签（自绘：选中页画成强调色主按钮）----
    sink = nullptr;
    makeControl(L"BUTTON", L"扇区动作",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
                0, kTabLeft, 14, 96, kButtonHeight, kTabSectorsId);
    makeControl(L"BUTTON", L"黑名单",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kTabLeft + 104, 14, 96, kButtonHeight, kTabProfilesId);

    // ==================== 第 1 页：扇区动作 ====================
    sink = &state->sectorPageControls;

    makeLabel(L"扇区数", kRightLeft, 44, 44, 18);
    HWND countCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        kRightLeft + 48, 40, 90, 160, kSectorCountComboId);
    if (countCombo != nullptr) {
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"4 扇区"));
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"6 扇区"));
        SendMessageW(countCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"8 扇区"));
        SendMessageW(countCombo, CB_SETCURSEL, 2, 0);
    }

    HWND sectorList = makeControl(
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        WS_EX_CLIENTEDGE, kRightLeft, kSectorListTop, kRightWidth,
        kSectorListHeight, kSectorListId);
    if (sectorList != nullptr) {
        ListView_SetExtendedListViewStyle(
            sectorList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        struct ColumnSpec {
            const wchar_t* text;
            int width;
        };
        // 「目标」列吃满剩余宽度：列表加宽后固定列宽会在右边留一条空白，
        // 视觉上像多了一列空表头。
        const ColumnSpec columns[] = {
            {L"扇区", 92},
            {L"类型", 84},
            {L"名称", 120},
            {L"目标", kRightWidth - 92 - 84 - 120 - 8},
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

    // ---- 动作表单 ----
    const int formTop = kSectorListTop + kSectorListHeight + 12;  // 296
    makeLabel(L"类型", kRightLeft, formTop + 4, 30, 18);
    HWND typeCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        kRightLeft + 32, formTop, 100, 200, kActionTypeComboId);
    if (typeCombo != nullptr) {
        for (int i = 0; i < kActionTypeCount; ++i) {
            SendMessageW(typeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kActionTypes[i].label));
        }
        SendMessageW(typeCombo, CB_SETCURSEL, 0, 0);
    }

    makeLabel(L"名称", kRightLeft + 144, formTop + 4, 32, 18);
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, kRightLeft + 180, formTop - 2, 140, 24,
                kActionNameEditId);

    makeLabel(L"修饰键", kRightLeft + 332, formTop + 4, 44, 18);
    HWND modifierCombo = makeControl(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0,
        kRightLeft + 380, formTop, 90, 160, kActionModifierComboId);
    if (modifierCombo != nullptr) {
        for (int i = 0; i < kModifierCount; ++i) {
            SendMessageW(modifierCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kModifiers[i].label));
        }
        SendMessageW(modifierCombo, CB_SETCURSEL, 0, 0);
    }

    const int targetTop = formTop + 34;
    makeLabel(L"目标", kRightLeft, targetTop + 4, 30, 18);
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, kRightLeft + 32, targetTop - 2, 300, 24,
                kActionTargetEditId);
    makeControl(L"BUTTON", L"选择…",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kRightLeft + 340, targetTop - 2, 64, kSmallButtonHeight,
                kActionPickId);

    const int keyTop = targetTop + 34;
    makeLabel(L"主键", kRightLeft, keyTop + 4, 30, 18);
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, kRightLeft + 32, keyTop - 2, 60, 24,
                kActionKeyEditId);
    makeControl(L"STATIC", L"例如 C / F4 / Space，仅「快捷键」类型生效。",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kRightLeft + 100,
                keyTop + 2, 380, 18, kActionKeyHintId);

    // 动态说明：选中扇区的状态提示（空槽位 / 需要选目标 / 快捷键会注入到前台）
    makeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kRightLeft,
                keyTop + 30, kRightWidth, 36, kActionStateHintId);

    // ==================== 第 2 页：黑名单 ====================
    sink = &state->profilePageControls;

    HWND profileList = makeControl(
        WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
        WS_EX_CLIENTEDGE, kMargin, 62, kDialogWidth - 2 * kMargin, 220,
        kBlacklistListId);
    if (profileList != nullptr) {
        ListView_SetExtendedListViewStyle(
            profileList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        column.pszText = const_cast<LPWSTR>(L"进程名（这些程序里不弹轮盘）");
        column.cx = scaleFor(hwnd, kDialogWidth - 2 * kMargin - 8);
        column.fmt = LVCFMT_LEFT;
        ListView_InsertColumn(profileList, 0, &column);
    }

    const int profileFormTop = 294;
    makeLabel(L"进程名", kMargin, profileFormTop + 4, 48, 18);
    makeControl(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                WS_EX_CLIENTEDGE, kMargin + 52, profileFormTop - 2, 260, 24,
                kBlacklistEditId);
    makeControl(L"BUTTON", L"添加 / 更新",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kMargin + 324, profileFormTop - 2, 110, kSmallButtonHeight,
                kBlacklistAddId);
    makeControl(L"BUTTON", L"删除选中",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kMargin + 442, profileFormTop - 2, 90, kSmallButtonHeight,
                kBlacklistDeleteId);

    // 说明按四行给高度：150% 缩放下这段中文会折到第四行，给少了末尾会被裁掉
    makeControl(
        L"STATIC",
        L"只按进程名整名匹配、不区分大小写（如 chrome.exe），不支持通配符。\r\n"
        L"命中后鼠标事件完全放行：该应用里的右键菜单、鼠标手势都不受影响。\r\n"
        L"少数 UWP 应用的进程名统一是 ApplicationFrameHost.exe，拉黑会连带"
        L"全部 UWP 应用。",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, kMargin, profileFormTop + 34,
        kDialogWidth - 2 * kMargin, 72, kBlacklistHintId);

    // ---- 底部按钮 ----
    sink = nullptr;
    const int buttonY = kDialogHeight - kMargin - kButtonHeight;
    makeControl(L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                kDialogWidth - kMargin - 2 * kButtonWidth - 8, buttonY,
                kButtonWidth, kButtonHeight, kOkId);
    makeControl(L"BUTTON", L"取消",
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
            // 说明文字不铺灰底，直接画在白色卡片上
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), dialog_style::text());
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
                        // resize 保留原有扇区，缩小时被裁掉的那几个直接丢弃。
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
                    MessageBoxW(hwnd, L"请先填写进程名，例如 game.exe。",
                                L"黑名单", MB_OK | MB_ICONINFORMATION);
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
                        MessageBoxW(hwnd, L"这个进程名已经在列表里了。",
                                    L"黑名单", MB_OK | MB_ICONINFORMATION);
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
                    MessageBoxW(hwnd, L"请先在列表中选择要删除的进程。",
                                L"黑名单", MB_OK | MB_ICONINFORMATION);
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

// ---------------------------------------------------------------- 对外入口

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
    // 手改文件写成 4 扇区却给 8 个动作等）。编辑器本身要求两者恒等。
    if (state.draft.menu.actions.size() != state.draft.menu.sectorCount ||
        state.draft.menu.sectorCount < core::kMinSectors) {
        state.draft.menu.sectorCount = 8;
        state.draft.menu.actions.assign(8, core::RadialAction());
    }

    RECT desired{0, 0, scaleFor(owner, kDialogWidth),
                 scaleFor(owner, kDialogHeight)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    HWND dialog = CreateWindowExW(exStyle, kEditorClass, L"轮盘菜单设置",
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

    // 本地模态循环（与设置窗口同一策略：GetMessage + IsDialogMessage，
    // 用 IsWindow 作退出条件，绝不发 WM_QUIT）
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
