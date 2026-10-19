#include "ui/item_editor_dialog.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/unique_handles.h"
#include "ui/builtin_item_dialog.h"

#include <Shlwapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "msimg32.lib")  // AlphaBlend

namespace ktile {
namespace ui {
namespace {

// ---------------------------------------------------------------- 布局常量
// 单位均为 DIP（96dpi 像素），绘制时按 DPI 缩放。
//
// 布局分两列：
//   左列（x=20..200）  —— 图标/磁贴预览 + 磁贴大小 + 换图按钮
//   右列（x=212..600） —— 名称/目标/参数/工作目录/所属页面/管理员
//
// 左列存在的意义：图标是磁贴上最显眼的东西，改完能立刻看到结果，
// 比「填一个 .ico 路径」直观得多。

constexpr int kClientWidth = 620;
constexpr int kClientHeight = 430;

// 左列
constexpr int kPreviewX = 20;
constexpr int kPreviewY = 16;
constexpr int kPreviewWidth = 180;
constexpr int kPreviewHeight = 180;

constexpr int kSizeLabelY = 206;
constexpr int kSizeComboX = 88;
constexpr int kSizeComboY = 204;
constexpr int kSizeComboWidth = 112;

constexpr int kResetIconX = 20;
constexpr int kResetIconY = 232;
constexpr int kResetIconWidth = 180;
constexpr int kResetIconHeight = 26;

// 标题显示方式（跟随全局 / 始终显示 / 移入时显示）。
// 放在「磁贴大小」下面：两者都是「这块磁贴长什么样」的选项，
// 改完立刻反映在正上方的预览里。
constexpr int kLabelModeLabelY = 264;
constexpr int kLabelModeComboX = 88;
constexpr int kLabelModeComboY = 262;
constexpr int kLabelModeComboWidth = 112;

// 图标大小（每块磁贴独立覆盖全局设置）。放在「标题显示」下面，
// 同属「这块磁贴长什么样」的一组；滑块初值跟随全局，勾选「按统一设置」
// 时置灰。
constexpr int kIconScaleCheckY = 294;
constexpr int kIconScaleSliderY = 318;
constexpr int kIconScaleSliderWidth = 64;   // 88..152，别越过右列 x=212
constexpr int kIconScaleValueX = 158;

// 右列
constexpr int kLabelX = 212;
constexpr int kLabelWidth = 78;
constexpr int kLabelHeight = 20;

constexpr int kFieldX = 294;
constexpr int kFieldWidth = 306;
constexpr int kFieldHeight = 23;

constexpr int kBrowseFileX = 294;
constexpr int kBrowseDirX = 392;
constexpr int kBuiltinPickX = 492;   // 「系统项...」排在两个浏览按钮右侧
constexpr int kBrowseWidth = 92;
constexpr int kBrowseHeight = 25;

// 控件 ID（沿用 HLaunch 的编号习惯，便于对照）
constexpr int kNameId = 1001;
// 1002 原为「类型」下拉框 —— 已移除，类型改为按目标自动推断
constexpr int kTargetId = 1003;
constexpr int kArgumentsId = 1004;
constexpr int kWorkingDirectoryId = 1005;
// 1006 原为「图标路径」输入框 —— 已移除，改为顶部预览 + 点击换图
constexpr int kTabId = 1007;
constexpr int kAdministratorId = 1008;
// 1009/1010/1011 原为三个「浏览...」按钮，现拆成文件/目录两个 + 工作目录一个
constexpr int kBrowseTargetFileId = 1009;
constexpr int kBrowseWorkingDirectoryId = 1010;
constexpr int kBrowseTargetDirectoryId = 1011;
constexpr int kPreviewId = 1012;     // 自绘预览（SS_OWNERDRAW | SS_NOTIFY）
constexpr int kTileSizeId = 1013;    // 磁贴大小下拉
constexpr int kResetIconId = 1014;   // 「重置图标」
// 1015 原为「类型」只读提示行 —— 已按反馈移除，类型本来就不需要用户关心
constexpr int kLabelModeId = 1016;   // 标题显示方式下拉
constexpr int kBuiltinPickId = 1017; // 「系统项...」— 选内置命令/系统位置
constexpr int kIconScaleCheckId = 1018;  // 「按统一设置」勾选框
constexpr int kIconScaleSliderId = 1019; // 图标大小滑块（50~100）
constexpr int kIconScaleValueId = 1020;  // 图标大小数值显示
// 保存/取消直接用 IDOK / IDCANCEL：IsDialogMessageW 处理回车/Esc 时
// 对普通窗口发送的就是这两个 ID，按钮点击也发同 ID，一套处理两处来源。

constexpr const wchar_t* kEditorDialogClass = L"KTile.ItemEditorDialog";

// 对话框关闭通知（结束嵌套模态循环，不影响主消息循环）。
// 与 settings_window / 重命名对话框同一机制：PostQuitMessage 投递的是
// 线程级粘性 WM_QUIT，会连带杀掉主窗口的消息循环，绝不能用。
constexpr UINT kEditorDialogCloseMessage = WM_APP + 0x90U;

// 预览用配色。磁贴在真实窗口里是「半透明深色玻璃叠在亚克力背景上」，
// 对话框里没有背景模糊可叠，直接用中深灰模拟观感。
constexpr COLORREF kPreviewTileFill = RGB(0x3A, 0x3A, 0x3E);
constexpr COLORREF kPreviewTileBorder = RGB(0x62, 0x62, 0x68);
constexpr COLORREF kPreviewFrame = RGB(0x9A, 0x9A, 0x9A);
constexpr COLORREF kPreviewPlaceholder = RGB(0x8A, 0x8A, 0x8A);

// 标签底衬：与 launcher_window.cpp 的 kLabelScrim 同值（55% 黑）。
// 预乘形式，直接喂给 AlphaBlend。
constexpr std::uint32_t kLabelScrimPremultiplied = 0x8C000000U;

// ---------------------------------------------------------------- 小工具

int scaleFor(HWND window, int value) {
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

std::wstring controlText(HWND control) {
    if (control == nullptr) {
        return std::wstring();
    }
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, &value[0], length + 1);
    value.resize(static_cast<std::size_t>(copied > 0 ? copied : 0));
    return value;
}

std::wstring trimmed(std::wstring value) {
    const std::wstring whitespace = L" \t\r\n";
    const std::size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

// 把一块区域用「预乘 ARGB 纯色」半透明覆盖（GDI 里做局部 alpha 混合）。
// 用于磁贴预览上的标签底衬 —— 底衬之下既可能是磁贴底色，也可能是
// 图标像素，靠 FillRect 填不出正确的混合结果。
void blendSolidRect(HDC target, const RECT& rect,
                    const std::uint32_t premultipliedArgb) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1;
    info.bmiHeader.biHeight = -1;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP pixel = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
                                    nullptr, 0);
    if (pixel == nullptr || bits == nullptr) {
        if (pixel != nullptr) {
            DeleteObject(pixel);
        }
        return;
    }
    *static_cast<std::uint32_t*>(bits) = premultipliedArgb;

    HDC memory = CreateCompatibleDC(target);
    if (memory != nullptr) {
        HGDIOBJ old = SelectObject(memory, pixel);
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(target, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, memory, 0, 0, 1, 1, blend);
        SelectObject(memory, old);
        DeleteDC(memory);
    }
    DeleteObject(pixel);
}

// 目标路径 → 类型。推断不出时返回 nullopt（调用方保留原类型）。
//
// 「判断不出」的典型情形：没有扩展名、也不存在的短名字 ——
// 既可能是待创建的文件夹、也可能是裸命令名，还可能是没写协议的域名
// （office.farxin.cn）。这种情况硬猜成「文件」会悄悄改坏磁贴行为
// （右键菜单的文案、图标、是否允许「打开文件所在位置」都看类型），
// 所以宁可返回 nullopt 让调用方沿用原值。
std::optional<core::ItemType> inferItemTypeImpl(const std::wstring& target) {
    const std::wstring path = trimmed(target);
    if (path.empty()) {
        return std::nullopt;
    }
    // 内置命令（"ktile:lock"）长得像 URL（带冒号前缀），PathIsURLW
    // 会把它认成网址 —— 提前拦下，按应用处理（内置命令的执行语义
    // 与应用一致：点击就跑）。
    if (!core::builtinIdFromTarget(path).empty()) {
        return core::ItemType::Application;
    }
    if (PathIsURLW(path.c_str()) != FALSE) {
        return core::ItemType::Url;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            return core::ItemType::Folder;
        }
    }

    const wchar_t* extension = PathFindExtensionW(path.c_str());
    const bool hasExtension = extension != nullptr && *extension != L'\0';
    if (hasExtension) {
        if (_wcsicmp(extension, L".exe") == 0 ||
            _wcsicmp(extension, L".com") == 0 ||
            _wcsicmp(extension, L".bat") == 0 ||
            _wcsicmp(extension, L".cmd") == 0 ||
            _wcsicmp(extension, L".lnk") == 0) {
            return core::ItemType::Application;
        }
    }

    // 像不像一条「路径」：带目录分隔符，或带盘符（C:）。
    // 不像路径又没有扩展名判断依据的（"notepad" / "office.farxin.cn"）
    // 一律交给调用方沿用原类型。
    const bool looksLikePath =
        path.find(L'\\') != std::wstring::npos ||
        path.find(L'/') != std::wstring::npos ||
        (path.size() >= 2 && path[1] == L':');

    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return core::ItemType::File;          // 存在的文件
    }
    if (hasExtension && looksLikePath) {
        return core::ItemType::File;          // 还没创建的文件（含相对路径）
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- 图标预览
//
// 预览要回答两个问题：
//   1. 换成这张图后，磁贴上看起来是什么样
//   2. 磁贴大小换成大/中/小之后，图标还剩多少空间给标题
// 所以它不是「图片查看器」，而是「一块按真实比例画出来的磁贴」。

struct PreviewLayout {
    RECT tile{};
    RECT icon{};
    RECT label{};
    bool hasLabel = false;
};

// 依据磁贴矩形与尺寸档位推导图标/标签位置。
// 常量与 launcher_window.cpp 的 paintSingleTile 保持一致 ——
// 预览与真实磁贴用的是同一套几何规则，改了那边记得同步这里。
//
// fillIcon = 用「用户导入的自定义图片」：图片铺满整块磁贴内框（含 2px 内缩，
// 与 1x1 的「图标铺满」同一套规则）。系统图标仍是方框居中。
PreviewLayout computePreviewLayout(const RECT& tile, const core::TileSize size,
                                   const bool fillIcon) {
    constexpr float labelHeight = 18.0F;
    constexpr float iconLabelGap = 3.0F;
    constexpr float iconTopGap = 3.0F;
    constexpr float bottomInset = 3.0F;

    const float left = static_cast<float>(tile.left);
    const float top = static_cast<float>(tile.top);
    const float width = static_cast<float>(tile.right - tile.left);
    const float height = static_cast<float>(tile.bottom - tile.top);

    const float spanColumns = static_cast<float>(core::tileSpanColumns(size));
    const float spanRows = static_cast<float>(core::tileSpanRows(size));
    const bool isWide = (spanColumns > 1.0F && spanRows == 1.0F);
    const bool isTall = (spanColumns == 1.0F && spanRows > 1.0F);
    const bool isSmall = (spanColumns == 1.0F && spanRows == 1.0F);

    const float bottomLabelTop = top + height - bottomInset - labelHeight;

    // 自定义图片：整块磁贴内框都给图片（标签叠在图上，与 1x1 的做法一致）
    if (fillIcon) {
        PreviewLayout filled;
        filled.tile = tile;
        filled.icon = RECT{tile.left + 2, tile.top + 2, tile.right - 2,
                           tile.bottom - 2};
        filled.label = RECT{tile.left + 1,
                            static_cast<LONG>(std::lround(bottomLabelTop)),
                            tile.right - 1,
                            static_cast<LONG>(std::lround(bottomLabelTop +
                                                          labelHeight))};
        return filled;
    }

    float side = 16.0F;
    float iconLeft = left;
    float iconTop = top;
    float labelTop = bottomLabelTop;

    if (isWide) {
        side = (std::max)(12.0F,
                          (std::min)(height - iconTopGap * 2.0F, width - 8.0F));
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + (height - side) / 2.0F;
    } else if (isSmall) {
        side = (std::max)(12.0F, (std::min)(width, height) - 4.0F);
        iconLeft = left + 2.0F;
        iconTop = top + 2.0F;
    } else if (isTall) {
        side = (std::max)(12.0F,
                          (std::min)(width - 8.0F,
                                     height - bottomInset - labelHeight -
                                         iconLabelGap - iconTopGap));
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + iconTopGap +
                  (height - iconTopGap - bottomInset - labelHeight -
                   iconLabelGap - side) /
                      2.0F;
    } else {
        const float maxIconHeight =
            height - iconTopGap - iconLabelGap - labelHeight - bottomInset;
        side = (std::max)(12.0F, (std::min)(width - 8.0F, maxIconHeight));
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + iconTopGap;
        labelTop = iconTop + side + iconLabelGap;
    }

    PreviewLayout layout;
    layout.tile = tile;
    layout.icon = RECT{static_cast<LONG>(std::lround(iconLeft)),
                       static_cast<LONG>(std::lround(iconTop)),
                       static_cast<LONG>(std::lround(iconLeft + side)),
                       static_cast<LONG>(std::lround(iconTop + side))};
    layout.label = RECT{tile.left + 1, static_cast<LONG>(std::lround(labelTop)),
                        tile.right - 1,
                        static_cast<LONG>(std::lround(bottomLabelTop +
                                                      labelHeight))};
    return layout;
}

// ---------------------------------------------------------------- 状态

struct EditorDialogState {
    ItemEditorFields initial;
    std::vector<std::wstring> tabNames;
    bool accepted = false;
    ItemEditorFields result;

    // 新建还是修改：只影响标题与确认按钮文案
    ItemEditorMode mode = ItemEditorMode::Edit;
    // 对话框标题（WM_CREATE 里建控件、出错弹框都要用，存一份免得两处硬编码）
    const wchar_t* title = L"修改项目";

    // ---- 预览状态 ----
    // 当前选中的自定义图标（存的是相对 data 的路径，空 = 用目标图标）
    std::wstring customIcon;
    core::TileSize previewSize = core::TileSize::Small;

    // 标题显示方式（下拉当前值）+ 设置里的「默认显示标题」。
    // 后者是给「跟随全局设置」那一档用的 —— 不知道全局值就画不出真实预览。
    core::TileLabelMode labelMode = core::TileLabelMode::FollowGlobal;
    bool globalAlwaysShowLabels = true;

    // 图标大小：本项覆盖值（0 = 按统一设置）+ 设置里的全局值（%）。
    int iconScalePercent = 0;
    int globalIconScale = 90;

    // 已解码的图标像素 + 供 AlphaBlend 使用的预乘 DIB。
    // pixelsKey 记录这些像素来自哪个来源，避免每次重绘都重新解码
    // （解码要走 WIC 开文件，几十毫秒级，放在 WM_PAINT 里会拖慢重绘）。
    platform::win32::ImagePixels pixels;
    std::wstring pixelsKey;
    platform::win32::UniqueHBITMAP previewDib;
    bool hasPixels = false;

    bool hasCustomIcon() const noexcept { return !customIcon.empty(); }
};

// 多行参数框：把 vector<wstring> 拼成 CRLF 分隔的文本
std::wstring joinArguments(const std::vector<std::wstring>& arguments) {
    std::wstring joined;
    for (const std::wstring& line : arguments) {
        if (!joined.empty()) {
            joined += L"\r\n";
        }
        joined += line;
    }
    return joined;
}

// 把多行文本拆回 vector（兼容 \r\n / \n / \r，空行丢弃）
std::vector<std::wstring> splitArguments(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring current;
    for (const wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            continue;
        }
        current += ch;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

// ---------------------------------------------------------------- 文件夹/文件浏览

// 用 SHBrowseForFolderW 而不是 IFileOpenDialog(FOS_PICKFOLDERS)：
// 两者都能选目录，但前者是纯 Win32 调用、不需要手动管 COM 接口生存期，
// 且 Win7 一路可用 —— 与本项目「最朴素的 Win32」风格一致。
// （进程已 CoInitializeEx(STA)，见 main.cpp。）
std::wstring browseForFolder(HWND owner, const wchar_t* title) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE |
                     BIF_EDITBOX | BIF_USENEWUI;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browse);
    if (pidl == nullptr) {
        return std::wstring();  // 用户取消
    }

    wchar_t path[MAX_PATH] = {};
    std::wstring result;
    if (SHGetPathFromIDListW(pidl, path) != FALSE) {
        result = path;
    }
    CoTaskMemFree(pidl);  // shell 分配的 PIDL 必须用 OLE 分配器归还
    return result;
}

std::wstring browseForFile(HWND owner,
                           const wchar_t* title,
                           const wchar_t* filter,
                           const wchar_t* defaultExtension) {
    wchar_t buffer[1024] = {};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(OPENFILENAMEW);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;  // 形如 L"描述\0模式\0描述\0模式\0\0"
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = title;
    dialog.lpstrDefExt = defaultExtension;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::wstring();  // 用户取消或出错
    }
    return buffer;
}

// ---------------------------------------------------------------- 控件创建

HWND addControl(HWND dialog, const wchar_t* windowClass, const wchar_t* text,
                DWORD style, int x, int y, int width, int height, int id,
                HFONT font) {
    HWND control = CreateWindowExW(
        0, windowClass, text, WS_CHILD | WS_VISIBLE | style,
        scaleFor(dialog, x), scaleFor(dialog, y),
        scaleFor(dialog, width), scaleFor(dialog, height),
        dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        nullptr, nullptr);
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return control;
}

void createControls(HWND dialog, EditorDialogState* state, HFONT font) {
    auto label = [&](const wchar_t* text, int x, int y, int width) {
        addControl(dialog, L"STATIC", text, 0, x, y + 2, width, kLabelHeight, 0,
                   font);
    };

    // ---- 左列：图标 / 磁贴预览 ----
    // SS_OWNERDRAW 让我们自己画；SS_NOTIFY 让点击（STN_CLICKED）
    // 能通知到父窗口 —— 这就是「点一下换图标」的实现方式。
    // 不给 WS_TABSTOP：预览进了 Tab 顺序后，Enter 会被 IsDialogMessageW
    // 当成「按下默认按钮」，用户想换图却把对话框保存了。
    addControl(dialog, L"STATIC", L"",
               SS_OWNERDRAW | SS_NOTIFY,
               kPreviewX, kPreviewY, kPreviewWidth, kPreviewHeight,
               kPreviewId, font);

    label(L"磁贴大小：", kPreviewX, kSizeLabelY, 68);
    HWND sizeCombo = addControl(dialog, L"COMBOBOX", L"",
                                CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                kSizeComboX, kSizeComboY, kSizeComboWidth,
                                160, kTileSizeId, font);
    for (int index = 0; index < 4; ++index) {
        SendMessageW(sizeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         core::tileSizeName(core::tileSizeFromInt(index))));
    }

    addControl(dialog, L"BUTTON", L"重置图标", WS_TABSTOP,
               kResetIconX, kResetIconY, kResetIconWidth, kResetIconHeight,
               kResetIconId, font);

    // ---- 左列：标题显示方式（每块磁贴单独控制）----
    label(L"标题显示：", kPreviewX, kLabelModeLabelY, 68);
    HWND labelCombo = addControl(dialog, L"COMBOBOX", L"",
                                 CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                 kLabelModeComboX, kLabelModeComboY,
                                 kLabelModeComboWidth, 150, kLabelModeId, font);
    for (int index = 0; index < 3; ++index) {
        SendMessageW(labelCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(core::tileLabelModeName(
                         core::tileLabelModeFromIndex(index))));
    }

    // ---- 左列：图标大小（每块磁贴单独覆盖全局设置）----
    addControl(dialog, L"BUTTON", L"按统一设置", BS_AUTOCHECKBOX | WS_TABSTOP,
               kPreviewX, kIconScaleCheckY, kResetIconWidth, 20,
               kIconScaleCheckId, font);
    addControl(dialog, L"STATIC", L"图标大小：", 0, kPreviewX,
               kIconScaleSliderY + 3, 68, kLabelHeight, 0, font);
    HWND scaleSlider = addControl(dialog, L"msctls_trackbar32", L"",
                                  WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                                  kSizeComboX, kIconScaleSliderY,
                                  kIconScaleSliderWidth, 24,
                                  kIconScaleSliderId, font);
    SendMessageW(scaleSlider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(50, 100));  // 50% ~ 100%，与设置窗口同一范围
    SendMessageW(scaleSlider, TBM_SETLINESIZE, 0, 5);
    SendMessageW(scaleSlider, TBM_SETPAGESIZE, 0, 10);
    addControl(dialog, L"STATIC", L"", 0, kIconScaleValueX,
               kIconScaleSliderY + 3, 44, kLabelHeight,
               kIconScaleValueId, font);

    // ---- 右列：名称 ----
    label(L"名称：", kLabelX, 20, kLabelWidth);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               kFieldX, 18, kFieldWidth, kFieldHeight, kNameId, font);

    // ---- 右列：目标 ----
    label(L"目标：", kLabelX, 53, kLabelWidth);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               kFieldX, 51, kFieldWidth, kFieldHeight, kTargetId, font);
    addControl(dialog, L"BUTTON", L"浏览文件...", WS_TABSTOP,
               kBrowseFileX, 80, kBrowseWidth, kBrowseHeight,
               kBrowseTargetFileId, font);
    addControl(dialog, L"BUTTON", L"浏览目录...", WS_TABSTOP,
               kBrowseDirX, 80, kBrowseWidth, kBrowseHeight,
               kBrowseTargetDirectoryId, font);
    // 「系统项…」只在新建时出现：内置命令是没法从文件系统浏览出来的，
    // 必须有入口；修改已有项目时目标已经定了，换目标该走删除重加。
    // 位置在两个浏览按钮右边 —— 曾与「浏览文件」同坐标叠放，按钮被盖住
    // 找不到，教训：同排控件坐标必须互斥。
    if (state->mode == ItemEditorMode::Create) {
        addControl(dialog, L"BUTTON", L"系统项...", WS_TABSTOP,
                   kBuiltinPickX, 80, kBrowseWidth, kBrowseHeight,
                   kBuiltinPickId, font);
    }

    // ---- 右列：参数（每行一个）----
    label(L"参数：", kLabelX, 130, kLabelWidth);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                   ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                   ES_WANTRETURN,
               kFieldX, 128, kFieldWidth, 78, kArgumentsId, font);

    // ---- 右列：工作目录 ----
    label(L"工作目录：", kLabelX, 214, kLabelWidth);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               kFieldX, 212, 210, kFieldHeight, kWorkingDirectoryId, font);
    addControl(dialog, L"BUTTON", L"浏览...", WS_TABSTOP,
               kFieldX + 220, 210, 86, kBrowseHeight,
               kBrowseWorkingDirectoryId, font);
    addControl(dialog, L"STATIC",
               L"程序运行时使用的起始文件夹；通常留空即可。",
               0, kFieldX, 239, kFieldWidth, 18, 0, font);

    // ---- 右列：所属页面 ----
    label(L"所属页面：", kLabelX, 264, kLabelWidth);
    HWND tabCombo = addControl(dialog, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               kFieldX, 262, 210, 160, kTabId, font);
    for (const std::wstring& tabName : state->tabNames) {
        SendMessageW(tabCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(tabName.c_str()));
    }

    // ---- 右列：管理员 ----
    addControl(dialog, L"BUTTON", L"以管理员身份运行",
               BS_AUTOCHECKBOX | WS_TABSTOP,
               kFieldX, 292, 220, 25, kAdministratorId, font);

    addControl(dialog, L"STATIC",
               L"名称留空时自动使用目标名称；参数每行一个。\n"
               L"文件夹磁贴可直接把路径粘贴到目标里。",
               0, kFieldX, 342, kFieldWidth, 34, 0, font);

    // ---- 保存 / 取消 ----
    // 新建时说「添加」而不是「保存」：用户心里做的是「加一个」，
    // 按钮跟他想做的事对齐，比通用词更少一分犹豫。
    const bool creating = state->mode == ItemEditorMode::Create;
    addControl(dialog, L"BUTTON", creating ? L"添加" : L"保存",
               WS_TABSTOP | BS_DEFPUSHBUTTON,
               420, kClientHeight - 48, 80, 28, IDOK, font);
    addControl(dialog, L"BUTTON", L"取消", WS_TABSTOP,
               510, kClientHeight - 48, 80, 28, IDCANCEL, font);

    // 输入长度上限（与 core 层校验一致，防极端输入）
    SendMessageW(GetDlgItem(dialog, kNameId), EM_SETLIMITTEXT, 256, 0);
    SendMessageW(GetDlgItem(dialog, kTargetId), EM_SETLIMITTEXT, 4096, 0);
    SendMessageW(GetDlgItem(dialog, kWorkingDirectoryId), EM_SETLIMITTEXT,
                 1024, 0);
}

// ---------------------------------------------------------------- 预览刷新

// 需要时重新解码图标像素。
//
// 来源只有两种：
//   自定义图标（用户选的图，存在 data\res）→ 直接解码该 PNG
//   目标自身（exe 的图标 / 文件夹图标 / 网页图标）→ 走 icon_loader
// 用 pixelsKey 做「来源指纹」，同一次打开对话框里只在来源变化时解码一次。
void updatePreviewPixels(HWND dialog, EditorDialogState* state) {
    const std::wstring target = trimmed(controlText(GetDlgItem(dialog, kTargetId)));
    const std::optional<core::ItemType> inferred = inferItemType(target);
    const core::ItemType type = inferred.value_or(state->initial.type);

    std::wstring key;
    if (state->hasCustomIcon()) {
        key = L"icon:" + platform::win32::resolveStoredIconPath(state->customIcon);
    } else {
        key = L"target:";
        key += target;
        key += (type == core::ItemType::Folder) ? L"|d" : L"|f";
        key += (type == core::ItemType::Url) ? L"|u" : L"|n";
    }
    if (key == state->pixelsKey) {
        return;
    }
    state->pixelsKey = key;
    state->hasPixels = false;

    if (state->hasCustomIcon()) {
        if (platform::win32::decodeImageFile(
                platform::win32::resolveStoredIconPath(state->customIcon),
                &state->pixels)) {
            state->hasPixels = true;
        }
        // 自定义图片读不出来（被删/被占用）时静默回落到目标图标，
        // 预览里看到的就是「保存前的样子」，不会突然变成空白
    }

    if (!state->hasPixels && !target.empty()) {
        const std::string targetUtf8 = base::wideToUtf8Lossy(target);
        platform::win32::UniqueHIcon icon = platform::win32::loadItemIcon(
            targetUtf8, std::string(),
            type == core::ItemType::Folder, type == core::ItemType::Url,
            platform::win32::IconSizeHint::Large);
        if (icon.valid() &&
            platform::win32::decodeIconPixels(icon.get(), &state->pixels)) {
            state->hasPixels = true;
        }
    }

    if (!state->hasPixels) {
        state->pixels.pixels.clear();
        state->pixels.width = 0;
        state->pixels.height = 0;
    }

    // 重建 AlphaBlend 用的 DIB：DIB 的 32bpp 数据必须是**预乘** alpha，
    // 否则半透明边缘会泛白（这是 AlphaBlend + AC_SRC_ALPHA 的硬性要求）。
    // 预乘规则与磁贴侧的 D2D 位图共用 icon_store 里的同一份实现。
    state->previewDib.reset();
    if (!state->hasPixels) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = state->pixels.width;
    info.bmiHeader.biHeight = -state->pixels.height;  // 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
                                  nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        state->hasPixels = false;
        return;
    }

    const std::vector<std::uint32_t> premultiplied =
        platform::win32::premultiplyBgra(state->pixels);
    std::memcpy(bits, premultiplied.data(),
                premultiplied.size() * sizeof(std::uint32_t));
    state->previewDib.reset(dib);
}

// 用「覆盖（cover）+ 居中裁剪」把图标画进目标矩形。
//
// 宁可裁掉两边，也不要留白或压扁。图的比例与目标框一致时（例如 1:2 的竖图
// 放进 1x2 磁贴）就是整张图铺满、一点都不裁；比例不符时才取源图中段。
// 注意这里的「目标框」对自定义图片而言是**整块磁贴**（见 fillIcon），
// 所以 128x128 的图放进 64x128 的磁贴时取的是源图中央的 64/128 竖条。
void drawIconCover(HDC target, const RECT& destination,
                   const EditorDialogState* state) {
    const int destWidth = destination.right - destination.left;
    const int destHeight = destination.bottom - destination.top;
    if (destWidth <= 0 || destHeight <= 0 || !state->hasPixels ||
        !state->previewDib.valid()) {
        return;
    }

    const int sourceWidth = state->pixels.width;
    const int sourceHeight = state->pixels.height;
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return;
    }

    // 目标宽高比比源图大 → 源图按宽度铺满，纵向裁中段；反之亦然
    const double scale = (std::max)(
        static_cast<double>(destWidth) / sourceWidth,
        static_cast<double>(destHeight) / sourceHeight);
    int cropWidth = static_cast<int>(std::lround(destWidth / scale));
    int cropHeight = static_cast<int>(std::lround(destHeight / scale));
    cropWidth = (std::min)((std::max)(cropWidth, 1), sourceWidth);
    cropHeight = (std::min)((std::max)(cropHeight, 1), sourceHeight);
    const int cropX = (sourceWidth - cropWidth) / 2;
    const int cropY = (sourceHeight - cropHeight) / 2;

    HDC memory = CreateCompatibleDC(target);
    if (memory == nullptr) {
        return;
    }
    HGDIOBJ old = SelectObject(memory, state->previewDib.get());
    SetStretchBltMode(target, HALFTONE);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    AlphaBlend(target, destination.left, destination.top, destWidth, destHeight,
               memory, cropX, cropY, cropWidth, cropHeight, blend);

    SelectObject(memory, old);
    DeleteDC(memory);
}

void paintPreview(HWND dialog, EditorDialogState* state, HDC dc,
                  const RECT& client) {
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
    if (state == nullptr) {
        return;
    }

    RECT frame = client;
    InflateRect(&frame, -1, -1);
    HBRUSH frameBrush = CreateSolidBrush(kPreviewFrame);
    if (frameBrush != nullptr) {
        FrameRect(dc, &frame, frameBrush);
        DeleteObject(frameBrush);
    }

    // ---- 计算磁贴的显示矩形：按真实像素尺寸画，居中于预览框 ----
    const int unit = scaleFor(dialog, 64);
    const int tileWidth = unit * core::tileSpanColumns(state->previewSize);
    const int tileHeight = unit * core::tileSpanRows(state->previewSize);
    const int availableWidth = client.right - client.left;
    const int availableHeight = client.bottom - client.top;

    double fit = 1.0;
    if (tileWidth + 12 > availableWidth) {
        fit = (availableWidth - 12) / static_cast<double>(tileWidth);
    }
    if (tileHeight + 12 > availableHeight) {
        fit = (std::min)(fit, (availableHeight - 12) /
                                  static_cast<double>(tileHeight));
    }
    const int drawnWidth =
        (std::max)(8, static_cast<int>(std::lround(tileWidth * fit)));
    const int drawnHeight =
        (std::max)(8, static_cast<int>(std::lround(tileHeight * fit)));

    RECT tile{};
    tile.left = client.left + (availableWidth - drawnWidth) / 2;
    tile.top = client.top + (availableHeight - drawnHeight) / 2;
    tile.right = tile.left + drawnWidth;
    tile.bottom = tile.top + drawnHeight;

    // 自定义图片（存在 data\res 的相对路径）铺满整块磁贴；
    // 用目标自身图标时仍是方框居中 —— 与 launcher_window.cpp 一致。
    const bool fillIcon =
        state->hasCustomIcon() &&
        platform::win32::isManagedIconPath(state->customIcon);

    const PreviewLayout layout =
        computePreviewLayout(tile, state->previewSize, fillIcon);

    // ---- 磁贴底 + 边框 ----
    const int radius = (std::max)(4, unit / 8);
    HBRUSH fillBrush = CreateSolidBrush(kPreviewTileFill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, kPreviewTileBorder);
    if (fillBrush != nullptr && borderPen != nullptr) {
        HGDIOBJ oldBrush = SelectObject(dc, fillBrush);
        HGDIOBJ oldPen = SelectObject(dc, borderPen);
        RoundRect(dc, tile.left, tile.top, tile.right, tile.bottom, radius * 2,
                  radius * 2);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    if (fillBrush != nullptr) {
        DeleteObject(fillBrush);
    }
    if (borderPen != nullptr) {
        DeleteObject(borderPen);
    }

    // ---- 图标 ----
    if (state->hasPixels) {
        drawIconCover(dc, layout.icon, state);
    } else {
        RECT placeholder = layout.icon;  // DrawTextW 需要可写矩形（会做 DT_CALCRECT 之类）
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kPreviewPlaceholder);
        DrawTextW(dc, L"无图标", -1, &placeholder,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // ---- 标题 ----
    std::wstring name = trimmed(controlText(GetDlgItem(dialog, kNameId)));
    if (name.empty()) {
        // 名称留空时显示层会回落到目标名 —— 预览也照这个来，避免「预览没标题、
        // 实际有标题」的错觉
        const std::wstring target =
            trimmed(controlText(GetDlgItem(dialog, kTargetId)));
        if (!target.empty()) {
            name = PathFindFileNameW(target.c_str());
        }
    }
    if (name.empty()) {
        return;
    }

    // ---- 标题是否画出来，取决于这块磁贴自己的设置 ----
    //
    // 预览画的是「安静状态」（鼠标不在磁贴上）的真实样子：
    //   始终显示 / 跟随全局(默认显示标题=开) → 标题在
    //   移入时显示 / 跟随全局(默认显示标题=关) → 标题不在
    // 所以选了「移入时显示」后预览里的标题会消失 —— 那不是坏了，
    // 而是如实告诉用户「平时它就长这样」。
    bool previewShowsLabel = state->globalAlwaysShowLabels;
    switch (state->labelMode) {
        case core::TileLabelMode::Always:
            previewShowsLabel = true;
            break;
        case core::TileLabelMode::OnHover:
            previewShowsLabel = false;
            break;
        case core::TileLabelMode::FollowGlobal:
        default:
            break;
    }
    if (!previewShowsLabel) {
        return;
    }

    RECT labelRect{};
    labelRect.left = layout.label.left;
    labelRect.top = layout.label.top;
    labelRect.right = layout.label.right;
    labelRect.bottom = layout.label.bottom;

    blendSolidRect(dc, labelRect, kLabelScrimPremultiplied);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT textRect = labelRect;
    textRect.left += 3;
    textRect.right -= 3;
    DrawTextW(dc, name.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
}

void refreshPreview(HWND dialog, EditorDialogState* state) {
    if (state == nullptr) {
        return;
    }
    updatePreviewPixels(dialog, state);
    HWND preview = GetDlgItem(dialog, kPreviewId);
    if (preview != nullptr) {
        InvalidateRect(preview, nullptr, FALSE);
    }
}

// 「类型」提示行已按反馈移除（类型由程序自动判断，不需要用户关心），
// inferItemType 仍用于保存时的类型决定。

void updateResetIconButton(HWND dialog, const EditorDialogState* state) {
    if (state == nullptr) {
        return;
    }
    // 没有自定义图标时这个按钮没有意义，置灰而不是藏起来
    // （位置固定，不会让界面跳动）
    EnableWindow(GetDlgItem(dialog, kResetIconId),
                 state->hasCustomIcon() ? TRUE : FALSE);
}

// ---------------------------------------------------------------- 交互

// 图标大小行的联动：勾选「按统一设置」→ 滑块置灰、数值显示全局值；
// 取消勾选 → 滑块恢复本项覆盖值（初值取全局，避免从 50 起步吓人）。
void syncIconScaleControls(HWND dialog, EditorDialogState* state) {
    const bool followGlobal =
        SendMessageW(GetDlgItem(dialog, kIconScaleCheckId), BM_GETCHECK, 0,
                     0) == BST_CHECKED;
    const int shown = followGlobal ? state->globalIconScale
                                   : state->iconScalePercent;
    EnableWindow(GetDlgItem(dialog, kIconScaleSliderId),
                 followGlobal ? FALSE : TRUE);
    SendMessageW(GetDlgItem(dialog, kIconScaleSliderId), TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::clamp(shown, 50, 100)));
    wchar_t text[16] = {};
    swprintf_s(text, L"%d%%", shown);
    SetDlgItemTextW(dialog, kIconScaleValueId, text);
}


void pickCustomIcon(HWND dialog, EditorDialogState* state) {
    const std::wstring selected = browseForFile(
        dialog, L"选择图标图片",
        L"图片文件\0*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
        L"所有文件\0*.*\0",
        nullptr);
    if (selected.empty()) {
        return;  // 用户取消
    }

    std::wstring error;
    const std::wstring stored =
        platform::win32::importCustomIcon(selected, &error);
    if (stored.empty()) {
        MessageBoxW(dialog, (L"无法使用该图片：\n" + error).c_str(),
                    L"更换图标", MB_OK | MB_ICONWARNING);
        return;
    }

    state->customIcon = stored;
    state->pixelsKey.clear();  // 强制重新解码
    refreshPreview(dialog, state);
    updateResetIconButton(dialog, state);
}

void populateControls(HWND dialog, EditorDialogState* state) {
    state->customIcon = state->initial.icon;
    state->previewSize = state->initial.tileSize;
    state->labelMode = state->initial.labelMode;

    SetDlgItemTextW(dialog, kNameId, state->initial.name.c_str());
    SetDlgItemTextW(dialog, kTargetId, state->initial.target.c_str());
    SetDlgItemTextW(dialog, kArgumentsId,
                    joinArguments(state->initial.arguments).c_str());
    SetDlgItemTextW(dialog, kWorkingDirectoryId,
                    state->initial.workingDirectory.c_str());

    SendMessageW(GetDlgItem(dialog, kTileSizeId), CB_SETCURSEL,
                 static_cast<WPARAM>(core::tileSizeToInt(state->previewSize)),
                 0);

    SendMessageW(GetDlgItem(dialog, kLabelModeId), CB_SETCURSEL,
                 static_cast<WPARAM>(core::tileLabelModeToIndex(
                     state->labelMode)),
                 0);

    HWND tabCombo = GetDlgItem(dialog, kTabId);
    const std::size_t tabCount = state->tabNames.size();
    const std::size_t selected =
        state->initial.tabIndex < tabCount ? state->initial.tabIndex
                                           : (tabCount > 0 ? tabCount - 1 : 0);
    SendMessageW(tabCombo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    // 只有一个页面时没有可选项，禁用下拉避免误导
    EnableWindow(tabCombo, tabCount > 1 ? TRUE : FALSE);

    SendMessageW(GetDlgItem(dialog, kAdministratorId), BM_SETCHECK,
                 state->initial.runAsAdministrator ? BST_CHECKED
                                                   : BST_UNCHECKED,
                 0);

    // 图标大小：0 = 按统一设置（勾选）；非 0 = 本项覆盖（滑块用它）
    const bool followGlobal = state->initial.iconScalePercent <= 0;
    SendMessageW(GetDlgItem(dialog, kIconScaleCheckId), BM_SETCHECK,
                 followGlobal ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!followGlobal) {
        state->iconScalePercent =
            std::clamp(state->initial.iconScalePercent, 50, 100);
    }
    syncIconScaleControls(dialog, state);

    updateResetIconButton(dialog, state);
    refreshPreview(dialog, state);
}

bool saveDialog(HWND dialog, EditorDialogState* state) {
    const std::wstring name = trimmed(controlText(GetDlgItem(dialog, kNameId)));
    const std::wstring target =
        trimmed(controlText(GetDlgItem(dialog, kTargetId)));

    // 目标必须有内容；名称允许留空（= 用目标名）
    if (target.empty()) {
        MessageBoxW(dialog,
                    state->mode == ItemEditorMode::Create
                        ? L"请先填写目标（程序、文件、文件夹或网址）。"
                        : L"目标不能为空。",
                    state->title, MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, kTargetId));
        return false;
    }

    // 类型不问了：按目标推断。推断不出（无扩展名且不存在）时沿用原类型 ——
    // 否则「只改个名字」会把 Application 悄悄改成 File。
    const std::optional<core::ItemType> inferred = inferItemType(target);

    state->result.name = name;
    state->result.target = target;
    state->result.type = inferred.value_or(state->initial.type);
    state->result.arguments =
        splitArguments(controlText(GetDlgItem(dialog, kArgumentsId)));
    state->result.workingDirectory =
        trimmed(controlText(GetDlgItem(dialog, kWorkingDirectoryId)));
    state->result.icon = state->customIcon;
    state->result.runAsAdministrator =
        SendMessageW(GetDlgItem(dialog, kAdministratorId), BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    state->result.tileSize = state->previewSize;
    state->result.labelMode = state->labelMode;

    // 图标大小：勾选「按统一设置」→ 0（跟随全局）；否则取滑块值
    const bool followGlobal =
        SendMessageW(GetDlgItem(dialog, kIconScaleCheckId), BM_GETCHECK, 0,
                     0) == BST_CHECKED;
    state->result.iconScalePercent =
        followGlobal
            ? 0
            : static_cast<int>(SendMessageW(GetDlgItem(dialog,
                                                        kIconScaleSliderId),
                                            TBM_GETPOS, 0, 0));

    const LRESULT tabSelection =
        SendMessageW(GetDlgItem(dialog, kTabId), CB_GETCURSEL, 0, 0);
    state->result.tabIndex =
        tabSelection == CB_ERR ? state->initial.tabIndex
                               : static_cast<std::size_t>(tabSelection);

    state->accepted = true;
    DestroyWindow(dialog);
    return true;
}

LRESULT CALLBACK editorDialogProc(HWND hwnd, UINT message,
                                  WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<EditorDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(
                                  createStruct->lpCreateParams));
            // 不能在这里 return TRUE：窗口标题是 DefWindowProcW 处理
            // WM_NCCREATE 时从 CREATESTRUCT 里存进去的。老代码直接
            // return TRUE，标题栏就一直空着（实测 GetWindowText 为空）。
            break;
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }
            HFONT font = dialogFont();
            createControls(hwnd, state, font);
            populateControls(hwnd, state);
            SetFocus(GetDlgItem(hwnd, kNameId));
            // 字体句柄由 WM_DESTROY 统一删除
            SetPropW(hwnd, L"KTileDialogFont", font);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw != nullptr && draw->CtlType == ODT_STATIC &&
                draw->CtlID == static_cast<UINT>(kPreviewId)) {
                paintPreview(hwnd, state, draw->hDC, draw->rcItem);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            switch (controlId) {
                case kPreviewId:
                    if (notification == STN_CLICKED && state != nullptr) {
                        pickCustomIcon(hwnd, state);
                    }
                    return 0;
                case kResetIconId:
                    if (state != nullptr) {
                        state->customIcon.clear();
                        state->pixelsKey.clear();
                        refreshPreview(hwnd, state);
                        updateResetIconButton(hwnd, state);
                    }
                    return 0;
                case kTileSizeId:
                    if (notification == CBN_SELCHANGE && state != nullptr) {
                        const LRESULT selection = SendMessageW(
                            GetDlgItem(hwnd, kTileSizeId), CB_GETCURSEL, 0, 0);
                        if (selection != CB_ERR) {
                            state->previewSize =
                                core::tileSizeFromInt(static_cast<int>(selection));
                            InvalidateRect(GetDlgItem(hwnd, kPreviewId), nullptr,
                                           FALSE);
                        }
                    }
                    return 0;
                case kLabelModeId:
                    if (notification == CBN_SELCHANGE && state != nullptr) {
                        const LRESULT selection = SendMessageW(
                            GetDlgItem(hwnd, kLabelModeId), CB_GETCURSEL, 0, 0);
                        if (selection != CB_ERR) {
                            state->labelMode = core::tileLabelModeFromIndex(
                                static_cast<int>(selection));
                            // 预览要跟着变：选了「移入时显示」后标题应从预览里
                            // 消失，否则用户看不出这个选项到底有没有生效
                            InvalidateRect(GetDlgItem(hwnd, kPreviewId), nullptr,
                                           FALSE);
                        }
                    }
                    return 0;
                case kBrowseTargetFileId: {
                    const std::wstring selected = browseForFile(
                        hwnd, L"选择文件或程序",
                        L"程序\0*.exe;*.com;*.bat;*.cmd;*.lnk\0"
                        L"所有文件\0*.*\0",
                        nullptr);
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kTargetId, selected.c_str());
                        // 浏览回来后必须主动刷预览：焦点此刻还在浏览按钮上，
                        // 编辑框既不会有 EN_CHANGE 之后的 EN_KILLFOCUS，
                        // EN_CHANGE 本身也只更新类型提示 —— 不补这一下，
                        // 左侧预览会一直停在旧图标（往往是空的占位框），
                        // 直到用户再点别处才「迟到」。
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                }
                case kBrowseTargetDirectoryId: {
                    const std::wstring selected = browseForFolder(
                        hwnd, L"选择文件夹");
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kTargetId, selected.c_str());
                        refreshPreview(hwnd, state);  // 同上
                    }
                    return 0;
                }
                case kBrowseWorkingDirectoryId: {
                    const std::wstring selected =
                        browseForFolder(hwnd, L"选择工作目录");
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kWorkingDirectoryId,
                                        selected.c_str());
                    }
                    return 0;
                }
                case kBuiltinPickId: {
                    // 选内置项目：名称/目标/图标三件一起回填。
                    // 名称也回填 —— 内置项的 target 要么是命令串要么是
                    // GUID 路径，名称派生逻辑对它们都会给出无意义的文本
                    // （GUID 串/命令名），必须在源头写好。
                    ui::BuiltinPickResult picked;
                    if (showBuiltinItemDialog(hwnd, &picked)) {
                        SetDlgItemTextW(hwnd, kTargetId,
                                        picked.target.c_str());
                        SetDlgItemTextW(hwnd, kNameId, picked.name.c_str());
                        state->customIcon = picked.icon;
                        // 图标换了，预览要重画（同浏览分支的理由）
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                }
                case kIconScaleCheckId:
                    if (state != nullptr) {
                        // 取消勾选瞬间，滑块要有合理的起步值：
                        // 本项有过覆盖值 → 恢复它；一直跟随全局 → 从全局值
                        // 起步（否则 state 里还是初值 0，会显示「0%」）
                        if (SendMessageW(GetDlgItem(hwnd, kIconScaleCheckId),
                                         BM_GETCHECK, 0, 0) == BST_UNCHECKED) {
                            state->iconScalePercent =
                                state->initial.iconScalePercent > 0
                                    ? std::clamp(
                                          state->initial.iconScalePercent, 50,
                                          100)
                                    : state->globalIconScale;
                        }
                        syncIconScaleControls(hwnd, state);
                    }
                    return 0;
                case kNameId:
                    // 名字边打边反映到预览的标题上
                    if (notification == EN_CHANGE) {
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                case kTargetId:
                    // 目标图标要等焦点离开才重新提取 ——
                    // 每敲一个字符就跑一次 shell 图标提取会明显卡顿
                    if (notification == EN_KILLFOCUS) {
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                case IDOK:
                    if (state != nullptr) {
                        saveDialog(hwnd, state);
                    }
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        }

        case WM_HSCROLL:
            // 滑块只有一个，来源必是图标大小（TRACKBAR 用 WM_HSCROLL 报位置）
            if (state != nullptr &&
                reinterpret_cast<HWND>(lParam) ==
                    GetDlgItem(hwnd, kIconScaleSliderId)) {
                state->iconScalePercent = static_cast<int>(
                    SendMessageW(GetDlgItem(hwnd, kIconScaleSliderId),
                                 TBM_GETPOS, 0, 0));
                syncIconScaleControls(hwnd, state);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            HFONT font =
                static_cast<HFONT>(RemovePropW(hwnd, L"KTileDialogFont"));
            if (font != nullptr) {
                DeleteObject(font);
            }
            // 预览用的 DIB 挂在 state 上，随对话框销毁一起走
            // 唤醒模态循环。绝不能用 PostQuitMessage —— 它投递的是
            // 线程级粘性 WM_QUIT，会连带终止主消息循环
            // （「设置窗口点确定后程序退出」就是它干的）。
            PostMessageW(hwnd, kEditorDialogCloseMessage, 0, 0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ensureEditorDialogClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &editorDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kEditorDialogClass;

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

std::optional<core::ItemType> inferItemType(const std::wstring& target) {
    return inferItemTypeImpl(target);
}

const wchar_t* itemTypeDisplayName(const core::ItemType type) {
    switch (type) {
        case core::ItemType::Application: return L"应用";
        case core::ItemType::File:        return L"文件";
        case core::ItemType::Folder:      return L"文件夹";
        case core::ItemType::Url:         return L"网址";
    }
    return L"应用";
}

bool showItemEditorDialog(HWND owner,
                          const std::vector<std::wstring>& tabNames,
                          const std::size_t initialTab,
                          const ItemEditorFields& initial,
                          const bool globalAlwaysShowLabels,
                          const int globalIconScale,
                          ItemEditorFields* outResult,
                          const ItemEditorMode mode) {
    if (outResult == nullptr || tabNames.empty()) {
        return false;
    }

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    ensureEditorDialogClass(instance);

    EditorDialogState state;
    state.initial = initial;
    state.initial.tabIndex = initialTab;
    state.tabNames = tabNames;
    state.previewSize = initial.tileSize;
    state.labelMode = initial.labelMode;
    state.globalAlwaysShowLabels = globalAlwaysShowLabels;
    state.globalIconScale = std::clamp(globalIconScale, 50, 100);
    state.mode = mode;
    state.title = (mode == ItemEditorMode::Create) ? L"添加项目" : L"修改项目";

    // 对话框居中于主窗口
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = scaleFor(owner, kClientWidth);
    const int height = scaleFor(owner, kClientHeight);
    const int x = ownerRect.left +
                  ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top +
                  ((ownerRect.bottom - ownerRect.top) - height) / 2;

    // 计算含边框的整体窗口尺寸（无菜单，hasMenu 传 FALSE）
    RECT desired{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    HWND dialog = CreateWindowExW(
        exStyle,
        kEditorDialogClass,
        state.title,
        style,
        x, y,
        desired.right - desired.left,
        desired.bottom - desired.top,
        owner,          // owner = 主窗口 → 点击对话框不会触发自动隐藏
        nullptr,
        instance,
        &state);

    if (dialog == nullptr) {
        return false;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环（与重命名/设置窗口同一套）：
    //  - 退出条件是 IsWindow(dialog)，不依赖 WM_QUIT；
    //  - 对话框自身的按键交给 IsDialogMessageW（Tab 切换、回车=保存、Esc=取消）；
    //  - 其余消息照常派发，主窗口在对话框期间依然活着。
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kEditorDialogCloseMessage) {
            continue;  // 唤醒用，不派发
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
    *outResult = state.result;
    return true;
}

}  // namespace ui
}  // namespace ktile
