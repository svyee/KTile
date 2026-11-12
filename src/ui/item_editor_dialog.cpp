#include "ui/item_editor_dialog.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "infrastructure/i18n.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/program_paths.h"
#include "platform/win32/unique_handles.h"
#include "ui/builtin_item_dialog.h"
#include "ui/color_picker.h"
#include "ui/dialog_style.h"

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

// --- 布局常量
// 单位均为 DIP（96dpi 像素），绘制时按 DPI 缩放。布局分两列：
// 左列图标/磁贴预览与外观选项，右列名称/目标等字段。
// 下列坐标是中文界面的基准值，实际值由 computeLayout 按实测文字递推。

// 基准值按中文界面定（标签列 68/78 恰好排下四个汉字）。英文标签更长，
// 定宽 STATIC 会直接切掉超宽文字，所以运行时由 computeLayout 先量再定宽：
// 长语言把标签列和整窗一起撑宽，字段宽度保持 306 不压缩，
// 压缩会让下方提示行重新折行。窗口宽度必须在 CreateWindowExW 之前定下。
constexpr int kClientWidth = 620;
constexpr int kClientHeight = 430;

// 内容卡片：所有输入控件包进一张白色圆角卡片，只影响背景绘制，
// 不改变控件坐标。
constexpr int kCardLeft = 12;
constexpr int kCardTop = 8;
constexpr int kCardRight = kClientWidth - 12;
constexpr int kCardBottom = 376;

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

// 标题显示方式（跟随全局 / 始终显示 / 移入时显示）：与磁贴大小同属
// 「这块磁贴长什么样」的选项，改完即时反映在正上方预览里。
constexpr int kLabelModeLabelY = 264;
constexpr int kLabelModeComboX = 88;
constexpr int kLabelModeComboY = 262;
constexpr int kLabelModeComboWidth = 112;

// 图标大小（本项覆盖全局设置），滑块初值跟随全局，勾「按统一设置」时置灰。
constexpr int kIconScaleCheckY = 294;
constexpr int kIconScaleSliderY = 318;
constexpr int kIconScaleSliderWidth = 64;   // 88..152，别越过右列 x=212
constexpr int kIconScaleValueX = 158;

// 背景色：文字 + 色块（点击选色）+「默认」按钮，属左列外观组。
constexpr int kTileColorLabelY = 346;
// 色块本身就是按钮与颜色样本，尺寸取原「选择」按钮的大小。
constexpr int kTileColorSwatchX = 80;
constexpr int kTileColorSwatchY = 345;
constexpr int kTileColorSwatchWidth = 56;
constexpr int kTileColorSwatchHeight = 24;
constexpr int kTileColorClearX = 142;
constexpr int kTileColorClearWidth = 36;

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

// 控件 ID（沿用 HLaunch 编号，跳号对应已移除的控件）
constexpr int kNameId = 1001;
constexpr int kTargetId = 1003;
constexpr int kArgumentsId = 1004;
constexpr int kWorkingDirectoryId = 1005;
constexpr int kTabId = 1007;
constexpr int kAdministratorId = 1008;
constexpr int kBrowseTargetFileId = 1009;
constexpr int kBrowseWorkingDirectoryId = 1010;
constexpr int kBrowseTargetDirectoryId = 1011;
constexpr int kPreviewId = 1012;     // 自绘预览（SS_OWNERDRAW | SS_NOTIFY）
constexpr int kTileSizeId = 1013;    // 磁贴大小下拉
constexpr int kResetIconId = 1014;   // 「重置图标」
constexpr int kLabelModeId = 1016;   // 标题显示方式下拉
constexpr int kBuiltinPickId = 1017; // 「系统项...」— 选内置命令/系统位置
constexpr int kIconScaleCheckId = 1018;  // 「按统一设置」勾选框
constexpr int kIconScaleSliderId = 1019; // 图标大小滑块（50~100）
constexpr int kIconScaleValueId = 1020;  // 图标大小数值显示
constexpr int kTileColorSwatchId = 1021; // 背景色样本（SS_OWNERDRAW）
constexpr int kTileColorClearId = 1023;  // 「默认」恢复跟随全局
// 提示行单独给 ID：WM_CTLCOLORSTATIC 靠 ID 区分该画次级灰的说明文字。
constexpr int kWorkingDirectoryHintId = 1030;
constexpr int kFooterHintId = 1031;
constexpr int kTargetHintId = 1032;
// 保存/取消直接用 IDOK / IDCANCEL：IsDialogMessageW 处理回车/Esc 时
// 对普通窗口发送的就是这两个 ID，一套处理两处来源。

constexpr const wchar_t* kEditorDialogClass = L"KTile.ItemEditorDialog";

// 对话框关闭通知，用于结束本地模态循环。绝不能用 PostQuitMessage：
// 它投递线程级粘性 WM_QUIT，会连带杀掉主窗口的消息循环。
constexpr UINT kEditorDialogCloseMessage = WM_APP + 0x90U;

// 预览用配色：真实磁贴是半透明深色玻璃叠亚克力，对话框里没有背景模糊，
// 用中深灰模拟。ARGB 形式供色块与「跟随全局」预览取默认。
constexpr std::uint32_t kPreviewTileFillArgb = 0xFF3A3A3E;
constexpr COLORREF kPreviewTileBorder = RGB(0x62, 0x62, 0x68);
constexpr COLORREF kPreviewFrame = RGB(0x9A, 0x9A, 0x9A);
constexpr COLORREF kPreviewPlaceholder = RGB(0x8A, 0x8A, 0x8A);

// 标签底衬：与 launcher_window.cpp 的 kLabelScrim 同值（55% 黑），预乘形式。
constexpr std::uint32_t kLabelScrimPremultiplied = 0x8C000000U;

// --- 小工具

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

// --- 布局计算
// 控件坐标全部在这里定，createControls 只按值摆放。窗口宽度取决于标签
// 实测宽度，而宽度必须在 CreateWindowExW 之前给出，所以测量发生在窗口
// 还不存在时：借 owner 的 DC 量（对话框开在 owner 上，DPI 相同）。
struct EditorLayout {
    int leftLabelW = 68;   // 左列标签列宽（磁贴大小 / 标题显示 / 图标大小共用）
    int colorLabelW = 56;  // 「背景色」标签列宽（更短，色块要贴着它）
    int sizeComboX = 88;
    int comboW = kSizeComboWidth;  // 左列两个下拉共用一宽
    int iconScaleValueX = 158;     // 滑块右侧的百分比数字
    int swatchX = 80;
    int clearX = 142;
    int clearW = kTileColorClearWidth;
    int labelX = kLabelX;
    int labelW = kLabelWidth;
    int fieldX = kFieldX;
    int fieldW = kFieldWidth;
    int browseFileX = kBrowseFileX;
    int browseDirX = kBrowseDirX;
    int builtinPickX = kBuiltinPickX;
    int workDirEditW = 210;
    int workDirBrowseX = kFieldX + 220;
    int okX = 420;
    int cancelX = 510;
    int clientWidth = kClientWidth;
    int clientHeight = kClientHeight;
    int cardRight = kCardRight;
};

EditorLayout computeLayout(HWND host, HFONT font) {
    auto text = [](const char* key) -> const wchar_t* {
        return infrastructure::i18n::trw(key).c_str();
    };
    // 用对话框字体现量文字，按 96 DPI 逻辑像素返回
    auto measure = [&](const wchar_t* value) -> int {
        if (value == nullptr || value[0] == L'\0' || font == nullptr) {
            return 0;
        }
        HDC dc = GetDC(host);
        if (dc == nullptr) {
            return 0;
        }
        HGDIOBJ previous = SelectObject(dc, font);
        SIZE size{};
        const bool measured =
            GetTextExtentPoint32W(dc, value, lstrlenW(value), &size) != FALSE;
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        SelectObject(dc, previous);
        ReleaseDC(host, dc);
        if (!measured || dpi <= 0) {
            return 0;
        }
        return MulDiv(size.cx, 96, dpi);
    };
    // 标签：实测宽 + 与控件之间的间隙，但不少于中文基准
    auto fitLabel = [&](const char* key, const int minWidth) -> int {
        return (std::max)(minWidth, measure(text(key)) + 8);
    };

    EditorLayout layout;

    // ---- 左列 ----
    layout.leftLabelW = (std::max)(layout.leftLabelW,
                                   fitLabel("editor.tileSize", 68));
    layout.leftLabelW = (std::max)(layout.leftLabelW,
                                   fitLabel("editor.labelMode", 68));
    layout.leftLabelW = (std::max)(layout.leftLabelW,
                                   fitLabel("editor.iconSize", 68));
    layout.sizeComboX = kPreviewX + layout.leftLabelW;
    // 滑块跟着下拉左沿走，百分比数字再接在滑块右边
    layout.iconScaleValueX =
        layout.sizeComboX + kIconScaleSliderWidth + 6;

    // 左列两个下拉共用一宽，取两边最长项。CBS_DROPDOWNLIST 选中项超宽
    // 直接截断不给省略号，英文项比中文宽一倍，必须实测。
    int comboW = kSizeComboWidth;
    for (int index = 0; index < 4; ++index) {
        comboW = (std::max)(comboW,
                            measure(text(core::tileSizeLabelKey(
                                        core::tileSizeFromInt(index)))) +
                                24);
    }
    for (int index = 0; index < 3; ++index) {
        comboW = (std::max)(comboW,
                            measure(text(core::tileLabelModeLabelKey(
                                        core::tileLabelModeFromIndex(index)))) +
                                24);
    }
    layout.comboW = comboW;

    // 左列右边界 = 预览块、下拉、背景色行三者取最大；
    // 「图标大小」的百分比数字允许略探出，右边还留着 10px。
    int leftRight = kPreviewX + kPreviewWidth;
    leftRight = (std::max)(leftRight, layout.sizeComboX + layout.comboW);

    layout.colorLabelW = fitLabel("editor.backgroundColor", 56);
    layout.swatchX = kPreviewX + layout.colorLabelW + 4;
    layout.clearX = layout.swatchX + kTileColorSwatchWidth + 6;
    // 「默认」按钮按文字加宽：中文两字塞进 36，英文 Default 会顶边
    layout.clearW =
        (std::max)(kTileColorClearWidth, measure(text("editor.defaultColor")) + 10);
    leftRight = (std::max)(leftRight, layout.clearX + layout.clearW);

    // ---- 右列 ----
    layout.labelX = leftRight + 12;
    layout.labelW = (std::max)(layout.labelW, fitLabel("editor.name", 78));
    layout.labelW = (std::max)(layout.labelW, fitLabel("editor.target", 78));
    layout.labelW = (std::max)(layout.labelW, fitLabel("editor.arguments", 78));
    layout.labelW = (std::max)(layout.labelW, fitLabel("editor.workDir", 78));
    layout.labelW = (std::max)(layout.labelW, fitLabel("editor.page", 78));
    layout.fieldX = layout.labelX + layout.labelW + 4;

    // 字段宽度不压：窄下去提示行会多折一行，被固定高度裁掉
    layout.fieldW = kFieldWidth;
    // 「浏览文件 / 浏览目录 / 系统项」按字段左沿递推
    layout.browseFileX = layout.fieldX;
    layout.browseDirX = layout.fieldX + 98;
    layout.builtinPickX = layout.fieldX + 198;
    // 工作目录行：输入框 + 右侧「浏览…」，右缘与字段右沿对齐
    layout.workDirEditW = layout.fieldW - 96;
    layout.workDirBrowseX = layout.fieldX + layout.fieldW - 86;

    // 标签列变宽就把整窗补上同样的宽度：内容整体右移，右边距不变
    const int grow = layout.fieldX - kFieldX;
    layout.clientWidth = kClientWidth + grow;
    layout.cardRight = layout.clientWidth - 12;
    layout.okX = 420 + grow;
    layout.cancelX = 510 + grow;
    return layout;
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

// 用预乘 ARGB 纯色做局部 alpha 混合（GDI 没有逐像素混合），
// 用于磁贴预览的标签底衬：底衬下可能是磁贴底色也可能是图标像素，
// FillRect 填不出正确的混合结果。
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

// 目标路径 → 类型。判断不出返回 nullopt（调用方保留原类型）：无扩展名
// 且不存在的短名字可能是文件夹、裸命令或域名，硬猜成「文件」会悄悄
// 改坏依赖类型的行为（右键菜单文案、图标等）。
std::optional<core::ItemType> inferItemTypeImpl(const std::wstring& target) {
    const std::wstring path = trimmed(target);
    if (path.empty()) {
        return std::nullopt;
    }
    // 内置命令（"ktile:lock"）带冒号前缀，PathIsURLW 会认成网址，
    // 提前拦下按应用处理（执行语义与应用一致）。
    if (!core::builtinIdFromTarget(path).empty()) {
        return core::ItemType::Application;
    }
    if (PathIsURLW(path.c_str()) != FALSE) {
        return core::ItemType::Url;
    }

    // 相对 exe 目录的路径（"tools\app.exe"）要先解析：否则按当前
    // 工作目录去找永远找不到，类型会被推断错。
    const std::wstring resolved =
        platform::win32::resolveProgramRelativePath(path);
    const DWORD attributes = GetFileAttributesW(resolved.c_str());
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

    // 像路径 = 带目录分隔符或盘符；不像路径又无扩展名判断依据的
    // （"notepad"、域名）交给调用方沿用原类型。
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

// --- 图标预览
// 预览是「按真实比例画出来的磁贴」而非图片查看器：
// 能直接看到换图、换尺寸后的真实效果。

struct PreviewLayout {
    RECT tile{};
    RECT icon{};
    RECT label{};
    bool hasLabel = false;
};

// 依据磁贴矩形与尺寸档位推导图标/标签位置。常量与公式与
// launcher_window.cpp 的 paintSingleTile 保持一致，改那边记得同步这里。
// fillIcon = 用户导入的图片铺满整块磁贴内框（含 2px 内缩）；系统图标方框居中。
PreviewLayout computePreviewLayout(const RECT& tile, const core::TileSize size,
                                   const bool fillIcon,
                                   const int iconScalePercent) {
    constexpr float labelHeight = 18.0F;
    constexpr float iconLabelGap = 3.0F;
    constexpr float iconTopGap = 3.0F;

    const float left = static_cast<float>(tile.left);
    const float top = static_cast<float>(tile.top);
    const float width = static_cast<float>(tile.right - tile.left);
    const float height = static_cast<float>(tile.bottom - tile.top);

    const float spanColumns = static_cast<float>(core::tileSpanColumns(size));
    const float spanRows = static_cast<float>(core::tileSpanRows(size));
    const bool isWide = (spanColumns > 1.0F && spanRows == 1.0F);
    const bool isTall = (spanColumns == 1.0F && spanRows > 1.0F);
    const bool isSmall = (spanColumns == 1.0F && spanRows == 1.0F);

    // 图标缩放：与 launcher_window.cpp 的 paintSingleTile 同一套公式
    const float iconFactor =
        static_cast<float>(std::clamp(iconScalePercent, 50, 100)) / 100.0F;

    // 标签底衬贴住磁贴下边缘（与 paintSingleTile 一致，不留缝隙）
    const float bottomLabelTop = top + height - labelHeight;

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

    // 统一规则（与 paintSingleTile 一致）：标签贴磁贴底边，
    // 图标在标签上方区域（或整格）内居中。
    if (isWide) {
        const float base =
            (std::min)(height - iconTopGap * 2.0F, width - 8.0F);
        side = (std::max)(12.0F, base * iconFactor);
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + (height - side) / 2.0F;
    } else if (isSmall) {
        const float base = (std::min)(width, height) - 4.0F;
        side = (std::max)(12.0F, base * iconFactor);
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + (height - side) / 2.0F;
    } else if (isTall) {
        const float base =
            (std::min)(width - 8.0F,
                       height - labelHeight - iconLabelGap - iconTopGap);
        side = (std::max)(12.0F, base * iconFactor);
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + iconTopGap +
                  (height - iconTopGap - labelHeight -
                   iconLabelGap - side) /
                      2.0F;
    } else {
        const float maxIconHeight =
            height - iconTopGap - iconLabelGap - labelHeight;
        const float base = (std::min)(width - 8.0F, maxIconHeight);
        side = (std::max)(12.0F, base * iconFactor);
        iconLeft = left + (width - side) / 2.0F;
        iconTop = top + iconTopGap + (maxIconHeight - side) / 2.0F;
        labelTop = bottomLabelTop;
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

// --- 状态

struct EditorDialogState {
    ItemEditorFields initial;
    // 插件磁贴的实际效果预览（app 层绑定主窗口的渲染路径）；非插件为空。
    PluginPreviewRenderer pluginPreview;
    std::vector<std::wstring> tabNames;
    bool accepted = false;
    ItemEditorFields result;

    // 布局与字体在窗口创建前算好：量文字要拿字体，坐标必须在
    // CreateWindowExW 之前定下，WM_CREATE 再建就晚了。销毁仍走 WM_DESTROY。
    EditorLayout layout;
    HFONT font = nullptr;

    // 新建还是修改：只影响标题与确认按钮文案
    ItemEditorMode mode = ItemEditorMode::Edit;
    // 对话框标题（WM_CREATE 里建控件、出错弹框都要用，存一份免得两处硬编码）
    const wchar_t* title = infrastructure::i18n::trw("editor.edit").c_str();

    // ---- 预览状态 ----
    // 当前选中的自定义图标（存的是相对 data 的路径，空 = 用目标图标）
    std::wstring customIcon;
    core::TileSize previewSize = core::TileSize::Small;

    // 标题显示方式当前值 + 设置里的全局值，后者供「跟随全局」档画真实预览。
    core::TileLabelMode labelMode = core::TileLabelMode::FollowGlobal;
    bool globalAlwaysShowLabels = true;

    // 图标大小：本项覆盖值（0 = 按统一设置）+ 设置里的全局值（%）。
    int iconScalePercent = 0;
    int globalIconScale = 90;

    // 磁贴底色：本项显式色（空 = 跟随全局）+ 全局默认色，
    // 预览与取色器初值都按这个优先级。
    std::optional<std::uint32_t> tileColor;
    std::optional<std::uint32_t> globalTileColor;

    // 已解码图标像素 + 预乘 DIB。pixelsKey 记录像素来源，不变不重复解码
    // （解码走 WIC 开文件，几十毫秒级，不能放进 WM_PAINT）。
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

// --- 取词与过滤器

const wchar_t* T(const char* key) {
    return infrastructure::i18n::trw(key).c_str();
}

// 合成 OPENFILENAMEW 过滤器串：描述\0模式\0…\0。描述来自语言文件，
// 只能逐段拼接；末尾双 NUL 是 Win32 约定，少一个会把后面的内存当过滤器读。
std::wstring buildFilter(const char* descriptionKey,
                         const wchar_t* patterns) {
    std::wstring filter;
    filter += infrastructure::i18n::trw(descriptionKey);
    filter.push_back(L'\0');
    filter += patterns;
    filter.push_back(L'\0');
    filter += infrastructure::i18n::trw("filter.allFiles");
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    return filter;
}

// --- 文件夹/文件浏览

// 用 SHBrowseForFolderW 而非 IFileOpenDialog(FOS_PICKFOLDERS)：纯 Win32、
// 不用管 COM 接口生存期，Win7 一路可用。进程已 CoInitializeEx(STA)，
// 见 main.cpp。
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

// --- 控件创建

HWND addControl(HWND dialog, const wchar_t* windowClass, const wchar_t* text,
                DWORD style, int x, int y, int width, int height, int id,
                HFONT font) {
    // 推按钮统一自绘：系统灰底压在白卡片上很扎眼。复选框保持原生
    // （自绘要重做键盘与视觉状态，收益不抵风险），灰底由 WM_CTLCOLORBTN 压成白底。
    if (windowClass != nullptr && wcscmp(windowClass, L"BUTTON") == 0) {
        const DWORD baseType = style & BS_TYPEMASK;
        if (baseType == BS_PUSHBUTTON || baseType == BS_DEFPUSHBUTTON) {
            style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
        }
    }
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
    const EditorLayout& layout = state->layout;
    auto label = [&](const wchar_t* text, int x, int y, int width) {
        addControl(dialog, L"STATIC", text, 0, x, y + 2, width, kLabelHeight, 0,
                   font);
    };

    // ---- 左列：图标 / 磁贴预览 ----
    // SS_OWNERDRAW 自绘；SS_NOTIFY 让点击（STN_CLICKED）通知父窗口，
    // 即「点一下换图标」。不给 WS_TABSTOP：预览进了 Tab 顺序后，
    // Enter 会被 IsDialogMessageW 当成按下默认按钮，直接保存了对话框。
    addControl(dialog, L"STATIC", L"",
               SS_OWNERDRAW | SS_NOTIFY,
               kPreviewX, kPreviewY, kPreviewWidth, kPreviewHeight,
               kPreviewId, font);

    label(infrastructure::i18n::trw("editor.tileSize").c_str(), kPreviewX, kSizeLabelY,
          layout.leftLabelW);
    HWND sizeCombo = addControl(dialog, L"COMBOBOX", L"",
                                CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                layout.sizeComboX, kSizeComboY, layout.comboW,
                                160, kTileSizeId, font);
    for (int index = 0; index < 4; ++index) {
        SendMessageW(sizeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         infrastructure::i18n::trw(
                             core::tileSizeLabelKey(
                                 core::tileSizeFromInt(index)))
                             .c_str()));
    }

    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.resetIcon").c_str(), WS_TABSTOP,
               kResetIconX, kResetIconY, kResetIconWidth, kResetIconHeight,
               kResetIconId, font);

    // ---- 左列：标题显示方式（每块磁贴单独控制）----
    label(infrastructure::i18n::trw("editor.labelMode").c_str(), kPreviewX, kLabelModeLabelY,
          layout.leftLabelW);
    HWND labelCombo = addControl(dialog, L"COMBOBOX", L"",
                                 CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                 layout.sizeComboX, kLabelModeComboY,
                                 layout.comboW, 150, kLabelModeId, font);
    for (int index = 0; index < 3; ++index) {
        SendMessageW(labelCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(infrastructure::i18n::trw(
                         core::tileLabelModeLabelKey(
                             core::tileLabelModeFromIndex(index)))
                                                  .c_str()));
    }

    // ---- 左列：图标大小（每块磁贴单独覆盖全局设置）----
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.followGlobal").c_str(), BS_AUTOCHECKBOX | WS_TABSTOP,
               kPreviewX, kIconScaleCheckY, kResetIconWidth, 20,
               kIconScaleCheckId, font);
    addControl(dialog, L"STATIC", infrastructure::i18n::trw("editor.iconSize").c_str(), 0, kPreviewX,
               kIconScaleSliderY + 3, layout.leftLabelW, kLabelHeight, 0, font);
    HWND scaleSlider = addControl(dialog, L"msctls_trackbar32", L"",
                                  WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                                  layout.sizeComboX, kIconScaleSliderY,
                                  kIconScaleSliderWidth, 24,
                                  kIconScaleSliderId, font);
    SendMessageW(scaleSlider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(50, 100));  // 50% ~ 100%，与设置窗口同一范围
    SendMessageW(scaleSlider, TBM_SETLINESIZE, 0, 5);
    SendMessageW(scaleSlider, TBM_SETPAGESIZE, 0, 10);
    addControl(dialog, L"STATIC", L"", 0, layout.iconScaleValueX,
               kIconScaleSliderY + 3, 44, kLabelHeight,
               kIconScaleValueId, font);

    // ---- 左列：背景色 ----
    // 色块（SS_NOTIFY 才能把点击发给父窗口）+「默认」清回全局。
    label(infrastructure::i18n::trw("editor.backgroundColor").c_str(), kPreviewX,
          kTileColorLabelY, layout.colorLabelW);
    addControl(dialog, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY,
               layout.swatchX, kTileColorSwatchY,
               kTileColorSwatchWidth, kTileColorSwatchHeight,
               kTileColorSwatchId, font);
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.defaultColor").c_str(), WS_TABSTOP,
               layout.clearX, kTileColorSwatchY, layout.clearW,
               kTileColorSwatchHeight, kTileColorClearId, font);

    // ---- 右列：名称 ----
    label(infrastructure::i18n::trw("editor.name").c_str(), layout.labelX, 20, layout.labelW);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               layout.fieldX, 18, layout.fieldW, kFieldHeight, kNameId, font);

    // ---- 右列：目标 ----
    label(infrastructure::i18n::trw("editor.target").c_str(), layout.labelX, 53, layout.labelW);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               layout.fieldX, 51, layout.fieldW, kFieldHeight, kTargetId, font);
    // 相对路径说明紧贴输入框：放在页脚提示里用户发现不了，改错了才回头读。
    addControl(dialog, L"STATIC",
               infrastructure::i18n::trw("editor.targetHint").c_str(),
               0, layout.fieldX, 108, layout.fieldW, 18, kTargetHintId, font);
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.browseFile").c_str(), WS_TABSTOP,
               layout.browseFileX, 80, kBrowseWidth, kBrowseHeight,
               kBrowseTargetFileId, font);
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.browseFolder").c_str(), WS_TABSTOP,
               layout.browseDirX, 80, kBrowseWidth, kBrowseHeight,
               kBrowseTargetDirectoryId, font);
    // 「系统项…」只在新建时出现：内置命令无法从文件系统浏览出来；
    // 修改已有项目时换目标该走删除重加。位置在两个浏览按钮右侧，
    // 同排控件坐标必须互斥，否则按钮会互相盖住。
    if (state->mode == ItemEditorMode::Create) {
        addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.systemItems").c_str(), WS_TABSTOP,
                   layout.builtinPickX, 80, kBrowseWidth, kBrowseHeight,
                   kBuiltinPickId, font);
    }

    // ---- 右列：参数（每行一个）----
    label(infrastructure::i18n::trw("editor.arguments").c_str(), layout.labelX, 130, layout.labelW);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                   ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                   ES_WANTRETURN,
               layout.fieldX, 128, layout.fieldW, 78, kArgumentsId, font);

    // ---- 右列：工作目录 ----
    label(infrastructure::i18n::trw("editor.workDir").c_str(), layout.labelX, 214, layout.labelW);
    addControl(dialog, L"EDIT", L"",
               WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
               layout.fieldX, 212, layout.workDirEditW, kFieldHeight, kWorkingDirectoryId, font);
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("common.browse").c_str(), WS_TABSTOP,
               layout.workDirBrowseX, 210, 86, kBrowseHeight,
               kBrowseWorkingDirectoryId, font);
    addControl(dialog, L"STATIC",
               infrastructure::i18n::trw("editor.workDirHint").c_str(),
               0, layout.fieldX, 239, layout.fieldW, 18, kWorkingDirectoryHintId,
               font);

    // ---- 右列：所属页面 ----
    label(infrastructure::i18n::trw("editor.page").c_str(), layout.labelX, 264, layout.labelW);
    HWND tabCombo = addControl(dialog, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               layout.fieldX, 262, 210, 160, kTabId, font);
    for (const std::wstring& tabName : state->tabNames) {
        SendMessageW(tabCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(tabName.c_str()));
    }

    // ---- 右列：管理员 ----
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("editor.admin").c_str(),
               BS_AUTOCHECKBOX | WS_TABSTOP,
               layout.fieldX, 292, 220, 25, kAdministratorId, font);

    addControl(dialog, L"STATIC",
               T("editor.hint"),
               0, layout.fieldX, 336, layout.fieldW, 34, kFooterHintId, font);

    // ---- 保存 / 取消 ----
    // 新建时按钮文案用「添加」，与用户想做的事一致。
    const bool creating = state->mode == ItemEditorMode::Create;
    addControl(dialog, L"BUTTON", creating ? infrastructure::i18n::trw("common.add").c_str() : infrastructure::i18n::trw("common.save").c_str(),
               WS_TABSTOP | BS_DEFPUSHBUTTON,
               layout.okX, layout.clientHeight - 48, 80, 28, IDOK, font);
    addControl(dialog, L"BUTTON", infrastructure::i18n::trw("common.cancel").c_str(), WS_TABSTOP,
               layout.cancelX, layout.clientHeight - 48, 80, 28, IDCANCEL, font);

    // 输入长度上限（与 core 层校验一致，防极端输入）
    SendMessageW(GetDlgItem(dialog, kNameId), EM_SETLIMITTEXT, 256, 0);
    SendMessageW(GetDlgItem(dialog, kTargetId), EM_SETLIMITTEXT, 4096, 0);
    SendMessageW(GetDlgItem(dialog, kWorkingDirectoryId), EM_SETLIMITTEXT,
                 1024, 0);
}

// --- 预览刷新

// 需要时重新解码图标像素。来源：自定义图标直接解码 PNG；
// 目标自身（exe/文件夹/网页图标）走 icon_loader。
// pixelsKey 是来源指纹，同一次打开只在来源变化时解码一次。
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
        // 预览保持「保存前的样子」，不会突然空白
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

    // DIB 的 32bpp 数据必须是预乘 alpha，否则半透明边缘泛白
    // （AlphaBlend + AC_SRC_ALPHA 的硬性要求）。预乘实现与磁贴侧
    // 的 D2D 位图共用 icon_store 里的同一份。
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

// 覆盖（cover）+ 居中裁剪：宁可裁边，不留白不压扁；比例一致时整图铺满。
// 目标框对自定义图片而言是整块磁贴（见 fillIcon），
// 所以竖图放进 1x2 磁贴时取的是源图中央的竖条。
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

    // ---- 磁贴的显示矩形：按真实像素尺寸画，居中于预览框 ----
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

    // 自定义图片铺满整块磁贴，系统图标方框居中（与 launcher_window.cpp 一致）；
    // 内置项自绘的 builtin-*.png 是方图，走缩放分支。
    const bool builtinIcon =
        state->hasCustomIcon() &&
        ([] (const std::wstring& icon) {
            const std::size_t slash = icon.find_last_of(L"\\/");
            const std::wstring filename =
                (slash == std::wstring::npos) ? icon : icon.substr(slash + 1);
            return filename.rfind(L"builtin-", 0) == 0;
        }(state->customIcon));
    const bool fillIcon =
        state->hasCustomIcon() &&
        platform::win32::isManagedIconPath(state->customIcon) &&
        !builtinIcon;

    // 预览的有效缩放值：勾「按统一设置」用全局值，否则用本项滑块值
    const bool followGlobal =
        SendMessageW(GetDlgItem(dialog, kIconScaleCheckId), BM_GETCHECK, 0,
                     0) == BST_CHECKED;
    const int effectiveScale =
        followGlobal ? state->globalIconScale : state->iconScalePercent;

    const PreviewLayout layout = computePreviewLayout(
        tile, state->previewSize, fillIcon, effectiveScale);

    // ---- 磁贴底 + 边框 ----
    // 底色按「本项显式色 > 全局默认色 > 预览默认灰」取，
    // 与主窗口 resolveTileBackground 同序，选完颜色预览立刻见效。
    const std::uint32_t previewTileColor =
        state->tileColor.has_value()
            ? *state->tileColor
            : state->globalTileColor.value_or(kPreviewTileFillArgb);
    const int radius = (std::max)(4, unit / 8);
    HBRUSH fillBrush = CreateSolidBrush(RGB((previewTileColor >> 16) & 0xFFU,
                                            (previewTileColor >> 8) & 0xFFU,
                                            previewTileColor & 0xFFU));
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

    // ---- 插件磁贴 ----
    // 没有目标图标可画，直接复用主窗口的插件渲染路径（布局 + D2D），
    // 用 GDI 重实现会逐渐偏离；回调失败时退回文字提示。
    if (state->initial.type == core::ItemType::Plugin &&
        state->pluginPreview) {
        if (state->pluginPreview(dc, tile, state->previewSize)) {
            return;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kPreviewPlaceholder);
        DrawTextW(dc, infrastructure::i18n::trw("editor.pluginPreviewUnavailable").c_str(), -1, &tile,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }

    // ---- 图标 ----
    if (state->hasPixels) {
        drawIconCover(dc, layout.icon, state);
    } else {
        RECT placeholder = layout.icon;  // DrawTextW 需要可写矩形（会做 DT_CALCRECT 之类）
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kPreviewPlaceholder);
        DrawTextW(dc, infrastructure::i18n::trw("editor.noIcon").c_str(), -1, &placeholder,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // ---- 标题 ----
    std::wstring name = trimmed(controlText(GetDlgItem(dialog, kNameId)));
    if (name.empty()) {
        // 名称留空时显示层回落到目标名，预览照做，避免「预览没标题、
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

    // ---- 标题是否画出来 ----
    // 预览画的是安静状态（鼠标不在磁贴上）的真实样子：
    // 始终显示或全局默认开 → 标题在；移入时显示或全局默认关 → 标题不在。
    // 选「移入时显示」后预览里的标题消失不是坏了，平时它就长这样。
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

// 「类型」提示行已移除（类型由程序判断，不需要用户关心），
// inferItemType 仍用于保存时的类型决定。

void updateResetIconButton(HWND dialog, const EditorDialogState* state) {
    if (state == nullptr) {
        return;
    }
    // 无自定义图标时置灰而不是藏起来，位置固定界面不跳
    EnableWindow(GetDlgItem(dialog, kResetIconId),
                 state->hasCustomIcon() ? TRUE : FALSE);
}

// --- 交互

// 图标大小行联动：勾「按统一设置」滑块置灰并显示全局值，
// 取消勾选恢复本项覆盖值（初值取全局，避免从 50 起步吓人）。
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
    const std::wstring iconFilter = buildFilter(
        "filter.iconImages", L"*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.gif");
    const std::wstring selected = browseForFile(
        dialog, T("editor.chooseIconTitle"), iconFilter.c_str(), nullptr);
    if (selected.empty()) {
        return;  // 用户取消
    }

    std::wstring error;
    const std::wstring stored =
        platform::win32::importCustomIcon(selected, &error);
    if (stored.empty()) {
        MessageBoxW(dialog,
                    (infrastructure::i18n::trw("editor.iconFailedPrefix") +
                     error).c_str(),
                    infrastructure::i18n::trw("editor.changeIconTitle").c_str(), MB_OK | MB_ICONWARNING);
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
    state->tileColor = state->initial.tileColor;

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
                        ? infrastructure::i18n::trw("editor.needTargetFirst").c_str()
                        : infrastructure::i18n::trw("editor.targetEmpty").c_str(),
                    state->title, MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, kTargetId));
        return false;
    }

    // 类型按目标推断；推断不出沿用原类型，
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
    // 空 = 跟随全局默认色；非空 = 本项显式色。每次保存完整提交，
    // 与尺寸、标题方式同一套「所见即所得」。
    state->result.tileColor = state->tileColor;

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
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            dialog_style::paintBackground(reinterpret_cast<HDC>(wParam),
                                          client);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc != nullptr) {
                RECT card{};
                card.left = scaleFor(hwnd, kCardLeft);
                card.top = scaleFor(hwnd, kCardTop);
                // 卡片宽度跟着标签列走：英文标签撑宽窗口时卡片一起变宽，
                // 否则右列控件掉出卡片
                card.right = scaleFor(hwnd, state != nullptr
                                                ? state->layout.cardRight
                                                : kCardRight);
                card.bottom = scaleFor(hwnd, kCardBottom);
                dialog_style::paintCard(dc, card, scaleFor(hwnd, 8));
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // 卡片内文字画在白底上；两处说明文字用次级灰，靠 ID 区分。
            HDC dc = reinterpret_cast<HDC>(wParam);
            const int controlId =
                GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            const bool isHint = controlId == kWorkingDirectoryHintId ||
                                controlId == kFooterHintId ||
                                controlId == kTargetHintId;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, isHint ? dialog_style::secondaryText()
                                    : dialog_style::text());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_CTLCOLORBTN: {
            // 复选框默认用 COLOR_BTNFACE 填底，在白卡片上是灰方块，压成白底。
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dialog_style::text());
            SetBkColor(dc, dialog_style::card());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_NCCREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(
                                  createStruct->lpCreateParams));
            // 这里不能 return TRUE：窗口标题由 DefWindowProcW 处理
            // WM_NCCREATE 时从 CREATESTRUCT 存入，提前返回标题栏一直是空的。
            break;
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }
            // 字体在 showItemEditorDialog 里建好（算坐标要用），
            // 这里只登记，销毁走 WM_DESTROY。
            HFONT font = state->font;
            createControls(hwnd, state, font);
            populateControls(hwnd, state);
            SetFocus(GetDlgItem(hwnd, kNameId));
            // 字体句柄由 WM_DESTROY 统一删除
            SetPropW(hwnd, L"KTileDialogFont", font);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw != nullptr && draw->CtlType == ODT_STATIC) {
                if (draw->CtlID == static_cast<UINT>(kPreviewId)) {
                    paintPreview(hwnd, state, draw->hDC, draw->rcItem);
                    return TRUE;
                }
                if (draw->CtlID == static_cast<UINT>(kTileColorSwatchId)) {
                    paintColorSwatch(
                        draw->hDC, draw->rcItem,
                        state != nullptr
                            ? state->tileColor
                            : std::optional<std::uint32_t>());
                    return TRUE;
                }
            }
            // 其余自绘按钮：保存/添加用强调色主按钮，其余白底细边。
            if (draw != nullptr && draw->CtlType == ODT_BUTTON) {
                wchar_t label[64] = {};
                GetWindowTextW(draw->hwndItem, label, 64);
                dialog_style::paintButton(
                    draw->hDC, draw->rcItem, label,
                    static_cast<HFONT>(GetPropW(hwnd, L"KTileDialogFont")),
                    draw->CtlID == static_cast<UINT>(IDOK)
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
                            // 预览要跟着变：选「移入时显示」后标题应从预览
                            // 消失，否则看不出选项是否生效
                            InvalidateRect(GetDlgItem(hwnd, kPreviewId), nullptr,
                                           FALSE);
                        }
                    }
                    return 0;
                case kBrowseTargetFileId: {
                    const std::wstring programFilter = buildFilter(
                        "filter.program", L"*.exe;*.com;*.bat;*.cmd;*.lnk");
                    const std::wstring selected = browseForFile(
                        hwnd, T("editor.chooseProgramTitle"),
                        programFilter.c_str(), nullptr);
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kTargetId, selected.c_str());
                        // 浏览回来必须主动刷预览：焦点还在浏览按钮上，
                        // 编辑框不会有 EN_KILLFOCUS，不刷就一直停在旧图标
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                }
                case kBrowseTargetDirectoryId: {
                    const std::wstring selected = browseForFolder(
                        hwnd, infrastructure::i18n::trw("editor.chooseFolderTitle").c_str());
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kTargetId, selected.c_str());
                        refreshPreview(hwnd, state);  // 同上
                    }
                    return 0;
                }
                case kBrowseWorkingDirectoryId: {
                    const std::wstring selected =
                        browseForFolder(hwnd, infrastructure::i18n::trw("editor.chooseWorkDirTitle").c_str());
                    if (!selected.empty()) {
                        SetDlgItemTextW(hwnd, kWorkingDirectoryId,
                                        selected.c_str());
                    }
                    return 0;
                }
                case kBuiltinPickId: {
                    // 名称/目标/图标一起回填；名称也回填，内置项的 target
                    // 是命令串或 GUID 路径，派生出的名称没有意义。
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
                        // 取消勾选瞬间滑块要有合理起步值：
                        // 有过覆盖值恢复它，一直跟随全局则从全局值起步
                        // （state 里还是初值 0，会显示「0%」）
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
                        // 统一/覆盖切换后有效缩放值变了，预览跟着刷
                        InvalidateRect(GetDlgItem(hwnd, kPreviewId), nullptr,
                                       TRUE);
                    }
                    return 0;
                case kTileColorSwatchId:
                    // 只认 STN_CLICKED：双击补发的 STN_DBLCLK 会弹两次色板。
                    if (state != nullptr && notification == STN_CLICKED) {
                        const std::uint32_t initial =
                            state->tileColor.value_or(
                                state->globalTileColor.value_or(
                                    kPreviewTileFillArgb));
                        // 内置色表：点一格立刻回填并刷新预览，弹板保持
                        // 打开，用户可在几块颜色间来回比较后直接保存。
                        showColorPalette(
                            hwnd, GetDlgItem(hwnd, kTileColorSwatchId),
                            initial,
                            [hwnd, state](const std::uint32_t color) {
                                state->tileColor = color;
                                InvalidateRect(
                                    GetDlgItem(hwnd, kTileColorSwatchId),
                                    nullptr, TRUE);
                                // 预览底色跟着换，用户不必先保存再回主窗口确认
                                refreshPreview(hwnd, state);
                            });
                    }
                    return 0;
                case kTileColorClearId:
                    if (state != nullptr) {
                        // 清回「跟随全局默认色」，不是清成固定灰
                        state->tileColor.reset();
                        InvalidateRect(GetDlgItem(hwnd, kTileColorSwatchId),
                                       nullptr, TRUE);
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                case kNameId:
                    // 名字边打边反映到预览的标题上
                    if (notification == EN_CHANGE) {
                        refreshPreview(hwnd, state);
                    }
                    return 0;
                case kTargetId:
                    // 目标图标等焦点离开才重新提取：
                    // 每敲一个字符跑一次 shell 图标提取会明显卡顿
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
                // 预览图标实时跟随滑块缩放
                InvalidateRect(GetDlgItem(hwnd, kPreviewId), nullptr, TRUE);
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
            // 预览用的 DIB 挂在 state 上随对话框销毁。用 PostMessageW 唤醒
            // 模态循环；绝不能用 PostQuitMessage，它投递线程级粘性 WM_QUIT
            // 会连带终止主消息循环。
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

// --- 对外接口

std::optional<core::ItemType> inferItemType(const std::wstring& target) {
    return inferItemTypeImpl(target);
}

const wchar_t* itemTypeDisplayName(const core::ItemType type) {
    switch (type) {
        case core::ItemType::Application: return T("editor.type.application");
        case core::ItemType::File:        return T("editor.type.file");
        case core::ItemType::Folder:      return T("editor.type.folder");
        case core::ItemType::Url:         return T("editor.type.url");
        case core::ItemType::Plugin:      return T("editor.type.plugin");
    }
    return T("editor.type.application");
}

bool showItemEditorDialog(HWND owner,
                          const std::vector<std::wstring>& tabNames,
                          const std::size_t initialTab,
                          const ItemEditorFields& initial,
                          const bool globalAlwaysShowLabels,
                          const int globalIconScale,
                          const std::optional<std::uint32_t>& globalTileColor,
                          ItemEditorFields* outResult,
                          const ItemEditorMode mode,
                          PluginPreviewRenderer pluginPreview) {
    if (outResult == nullptr || tabNames.empty()) {
        return false;
    }

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    ensureEditorDialogClass(instance);

    EditorDialogState state;
    state.initial = initial;
    state.pluginPreview = std::move(pluginPreview);
    state.initial.tabIndex = initialTab;
    state.tabNames = tabNames;
    state.previewSize = initial.tileSize;
    state.labelMode = initial.labelMode;
    state.globalAlwaysShowLabels = globalAlwaysShowLabels;
    state.globalIconScale = std::clamp(globalIconScale, 50, 100);
    state.globalTileColor = globalTileColor;
    state.mode = mode;
    state.title = (mode == ItemEditorMode::Create) ? infrastructure::i18n::trw("editor.create").c_str() : infrastructure::i18n::trw("editor.edit").c_str();

    // 对话框居中于主窗口
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    // 字体与控件坐标都在这里先算：窗口宽度取决于标签实测宽度，
    // 必须在 CreateWindowExW 之前给出。
    state.font = dialogFont();
    state.layout = computeLayout(owner, state.font);
    const int width = scaleFor(owner, state.layout.clientWidth);
    const int height = scaleFor(owner, state.layout.clientHeight);
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
        // 字体是这里建的，窗口没起来就没人能删它了
        if (state.font != nullptr) {
            DeleteObject(state.font);
        }
        return false;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环（与重命名/设置窗口同一套）：退出条件是 IsWindow(dialog)，
    // 不依赖 WM_QUIT；对话框自身按键交给 IsDialogMessageW（Tab/回车/Esc），
    // 其余消息照常派发，主窗口在对话框期间依然活着。
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
