// KTile - UI 层：「关于」对话框实现
#include "ui/about_dialog.h"

#include "infrastructure/i18n.h"
#include "ui/dialog_style.h"

#include <Shellapi.h>

#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// 客户区尺寸（DIP）。宽度要容得下「Github」「网盘下载」两个并排按钮。
constexpr int kClientWidth = 440;
constexpr int kClientHeight = 262;

constexpr int kMargin = 20;
constexpr int kIconSize = 48;
constexpr int kIconY = 22;
// 左侧文字列的起点：图标右侧留 16 间隙。
constexpr int kTextX = kMargin + kIconSize + 16;
constexpr int kTextWidth = kClientWidth - kTextX - kMargin;
constexpr int kTitleY = 22;
constexpr int kTitleHeight = 32;
constexpr int kVersionY = 58;
constexpr int kDescY = 80;
constexpr int kCopyrightY = 102;
// 分隔线与下方联系方式通铺整个宽度。
constexpr int kSeparatorY = 132;
constexpr int kEmailY = 148;
constexpr int kWechatY = 170;

constexpr int kButtonWidth = 84;
constexpr int kButtonHeight = 28;
constexpr int kButtonGap = 8;

// 白色圆角卡片：把标题 / 版本 / 联系方式包成一页，与设置窗口、
// 编辑窗口同一套视觉。底部的链接按钮与「确定」留在灰底上。
constexpr int kCardLeft = 12;
constexpr int kCardTop = 12;
constexpr int kCardRight = kClientWidth - 12;
constexpr int kCardBottom = 196;

// 控件 ID。确定按钮用 IDOK：回车（IsDialogMessageW）与点击同 ID，
// Esc 发 IDCANCEL —— 对纯展示对话框两者都等于「关闭」。
constexpr int kTitleId = 2001;
constexpr int kVersionId = 2002;
constexpr int kDescId = 2003;
constexpr int kCopyrightId = 2004;
constexpr int kSeparatorId = 2005;
constexpr int kEmailId = 2006;
constexpr int kWechatId = 2007;
// 发布地址 / 备用下载地址两个扁平按钮。
constexpr int kGithubId = 2008;
constexpr int kPanDownloadId = 2009;

constexpr const wchar_t* kGithubUrl =
    L"https://github.com/svyee/KTile/releases";
constexpr const wchar_t* kPanUrl =
    L"https://pan.baidu.com/s/1h7Cs_XQGaGMWqKnO8olbbQ?pwd=p3gf";

// 对话框关闭通知（结束本地模态循环）。与 text_input_dialog 的
// WM_APP+0x93 区分开，避免两条模态链路互相唤醒看不懂。
constexpr UINT kDialogCloseMessage = WM_APP + 0x94U;

// exe 内嵌的图标资源 ID（与 app.rc 的 101 ICON 对应）。
constexpr int kAppIconResourceId = 101;

struct DialogState {
    std::wstring productName;
    std::wstring version;
    // 由本对话框创建的字体，WM_DESTROY 时释放；系统 Stock 字体不在此列。
    HFONT createdBodyFont = nullptr;
    HFONT createdTitleFont = nullptr;
    // 自绘按钮要用的正文字体（可能是 Stock，不释放）。
    HFONT bodyFont = nullptr;
};

// 把 owner 的 DPI 换算成缩放系数（96 DPI = 1.0）。
// 不用 GetDpiForWindow：Win10 1607+ 才有，本项目要兼容 Win7 SP1。
double scaleFor(const HWND owner) {
    HDC dc = GetDC(owner);
    if (dc == nullptr) {
        return 1.0;
    }
    const int dpiX = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(owner, dc);
    if (dpiX <= 0) {
        return 1.0;
    }
    return static_cast<double>(dpiX) / 96.0;
}

int scaled(const double scale, const int value) {
    return static_cast<int>(value * scale + 0.5);
}

// 系统中是否安装了指定字体族。
//
// 为什么用枚举而不是直接 CreateFont：CreateFont 遇到不存在的族名会
// 静默替换成别的字体，没法判断是否命中；EnumFontFamiliesEx 按族名
// 过滤，只有字体真实存在时回调才会被调用。
bool hasFontFamily(const wchar_t* family) {
    const HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return false;
    }

    LOGFONTW filter{};
    filter.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(filter.lfFaceName, family, LF_FACESIZE);

    bool found = false;
    EnumFontFamiliesExW(
        dc, &filter,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// 关于对话框的界面字体：优先微软雅黑，其次思源黑体；都没装则返回
// nullptr，调用方回退系统默认字体。
//
// Win7 只有「Microsoft YaHei」，带 UI 后缀的版本从 Win8 才有，
// 所以两个名字都要探测；思源黑体各版本的族名有 SC / CN 两种写法。
const wchar_t* pickUiFontFamily() {
    static const wchar_t* const kCandidates[] = {
        L"Microsoft YaHei UI",
        L"Microsoft YaHei",
        L"Source Han Sans SC",
        L"Source Han Sans CN",
        L"思源黑体 CN",
        L"思源黑体 SC",
    };
    for (const wchar_t* family : kCandidates) {
        if (hasFontFamily(family)) {
            return family;
        }
    }
    return nullptr;
}

void finishDialog(const HWND dialog) {
    DestroyWindow(dialog);
}

// 用系统默认浏览器打开链接。失败不做提示：这只是辅助入口，
// 不应阻塞「确定」关闭流程。
void openUrl(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            // 必须 break 让 DefWindowProcW 存标题（同 text_input_dialog 的坑）。
            SetWindowLongPtrW(dialog, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(dialog, &client);
            dialog_style::paintBackground(reinterpret_cast<HDC>(wParam),
                                          client);
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(dialog, &paint);
            if (dc != nullptr) {
                const double scale = scaleFor(dialog);
                RECT card{};
                card.left = scaled(scale, kCardLeft);
                card.top = scaled(scale, kCardTop);
                card.right = scaled(scale, kCardRight);
                card.bottom = scaled(scale, kCardBottom);
                dialog_style::paintCard(dc, card, scaled(scale, 8));
            }
            EndPaint(dialog, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // 卡片内文字：标题与描述用正文色，版本 / 版权 / 联系方式
            // 用次级灰。返回卡片画刷，避免出现与卡片不一致的底色。
            HDC dc = reinterpret_cast<HDC>(wParam);
            const int controlId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            const bool isPrimary =
                controlId == kTitleId || controlId == kDescId;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, isPrimary ? dialog_style::text()
                                       : dialog_style::secondaryText());
            return reinterpret_cast<LRESULT>(dialog_style::cardBrush());
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw != nullptr && draw->CtlType == ODT_BUTTON) {
                auto* state = reinterpret_cast<DialogState*>(
                    GetWindowLongPtrW(dialog, GWLP_USERDATA));
                wchar_t label[64] = {};
                GetWindowTextW(draw->hwndItem, label, 64);
                dialog_style::paintButton(
                    draw->hDC, draw->rcItem, label,
                    state != nullptr ? state->bodyFont : nullptr,
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

        case WM_CREATE: {
            auto* state = reinterpret_cast<DialogState*>(
                GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (state == nullptr) {
                return -1;
            }

            const double scale = scaleFor(dialog);
            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            // 界面字体：优先使用系统已安装的微软雅黑 / 思源黑体；
            // 都没装时正文回退 DEFAULT_GUI_FONT、标题回退「微软雅黑 UI」
            //（CreateFont 会自动替换成可用字体，Win7 没有 UI 变体也能显示）。
            const wchar_t* uiFamily = pickUiFontFamily();

            HFONT bodyFont = static_cast<HFONT>(
                GetStockObject(DEFAULT_GUI_FONT));

            const int clientWidth = scaled(scale, kClientWidth);
            const int clientHeight = scaled(scale, kClientHeight);
            const int margin = scaled(scale, kMargin);
            const int iconSize = scaled(scale, kIconSize);
            const int textX = scaled(scale, kTextX);
            const int textWidth = scaled(scale, kTextWidth);
            const int buttonWidth = scaled(scale, kButtonWidth);
            const int buttonHeight = scaled(scale, kButtonHeight);

            // 应用图标：从 exe 资源取（app.rc 的 101 ICON）。
            // LR_SHARED：图标由系统缓存管理，不负责 DestroyIcon。
            const HICON appIcon = static_cast<HICON>(LoadImageW(
                instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
                iconSize, iconSize, LR_SHARED));
            if (appIcon != nullptr) {
                SendMessageW(dialog, WM_SETICON, ICON_SMALL,
                             reinterpret_cast<LPARAM>(appIcon));
                SendMessageW(dialog, WM_SETICON, ICON_BIG,
                             reinterpret_cast<LPARAM>(appIcon));
            }

            // 正文与标题同族：9pt 正文、14pt 加粗标题，按当前 DPI 计算字高。
            const HDC dc = GetDC(dialog);
            const int dpiY = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
            if (dc != nullptr) {
                ReleaseDC(dialog, dc);
            }
            const int effectiveDpi = dpiY > 0 ? dpiY : 96;

            if (uiFamily != nullptr) {
                state->createdBodyFont = CreateFontW(
                    -MulDiv(9, effectiveDpi, 72), 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                    uiFamily);
                if (state->createdBodyFont != nullptr) {
                    bodyFont = state->createdBodyFont;
                }
            }

            state->createdTitleFont = CreateFontW(
                -MulDiv(14, effectiveDpi, 72), 0, 0, 0, FW_BOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                uiFamily != nullptr ? uiFamily : L"Microsoft YaHei UI");
            const HFONT titleFont = state->createdTitleFont;
            state->bodyFont = bodyFont;

            // 版本行先拼成具名 wstring：数组里只能存 c_str()，
            // 直接内联临时对象会在表达式结束时就析构（悬垂）。
            const std::wstring versionLine = infrastructure::i18n::trw("about.version").c_str() + state->version;

            struct LineSpec {
                int id;
                int y;
                const wchar_t* text;
            };
            const LineSpec lines[] = {
                {kVersionId, kVersionY, versionLine.c_str()},
                {kDescId, kDescY, infrastructure::i18n::trw("about.description").c_str()},
                {kCopyrightId, kCopyrightY, L"© 2026 svyee · 奇竹科技"},
                {kEmailId, kEmailY, infrastructure::i18n::trw("about.email").c_str()},
                {kWechatId, kWechatY, infrastructure::i18n::trw("about.wechat").c_str()},
            };

            std::vector<HWND> children;
            children.reserve(10);

            const HWND title = CreateWindowExW(
                0, L"STATIC", state->productName.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                textX, scaled(scale, kTitleY), textWidth,
                scaled(scale, kTitleHeight), dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleId)),
                instance, nullptr);
            children.push_back(title);

            for (const LineSpec& line : lines) {
                const HWND control = CreateWindowExW(
                    0, L"STATIC", line.text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                    textX, scaled(scale, line.y), textWidth,
                    scaled(scale, 20), dialog,
                    reinterpret_cast<HMENU>(
                        static_cast<INT_PTR>(line.id)),
                    instance, nullptr);
                children.push_back(control);
            }

            const HWND separator = CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                margin, scaled(scale, kSeparatorY), clientWidth - margin * 2, 2,
                dialog,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kSeparatorId)),
                instance, nullptr);
            children.push_back(separator);

            // 发布地址 / 备用下载地址：扁平按钮，与「确定」同一行、靠左，
            // 点击后交给系统默认浏览器（不内嵌 WebView，保持零依赖）。
            const int linkButtonY = clientHeight - margin - buttonHeight;
            const HWND github = CreateWindowExW(
                0, L"BUTTON", L"Github",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                textX, linkButtonY, buttonWidth, buttonHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGithubId)),
                instance, nullptr);
            children.push_back(github);

            const HWND panDownload = CreateWindowExW(
                0, L"BUTTON", infrastructure::i18n::trw("about.download").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                textX + buttonWidth + scaled(scale, kButtonGap), linkButtonY,
                buttonWidth, buttonHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPanDownloadId)),
                instance, nullptr);
            children.push_back(panDownload);

            const HWND ok = CreateWindowExW(
                0, L"BUTTON", infrastructure::i18n::trw("common.ok").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                clientWidth - margin - buttonWidth,
                clientHeight - margin - buttonHeight, buttonWidth,
                buttonHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                instance, nullptr);
            children.push_back(ok);

            for (const HWND child : children) {
                if (child == nullptr) {
                    return -1;
                }
                SendMessageW(child, WM_SETFONT,
                             reinterpret_cast<WPARAM>(bodyFont), TRUE);
            }
            if (titleFont != nullptr) {
                SendMessageW(title, WM_SETFONT,
                             reinterpret_cast<WPARAM>(titleFont), TRUE);
            }

            SetFocus(ok);
            return 0;
        }

        case WM_COMMAND:
            // 确定 / 取消 / Esc / 标题栏关闭，统统等于关掉。
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                finishDialog(dialog);
            } else if (LOWORD(wParam) == kGithubId) {
                openUrl(kGithubUrl);
            } else if (LOWORD(wParam) == kPanDownloadId) {
                openUrl(kPanUrl);
            }
            return 0;

        case WM_CLOSE:
            finishDialog(dialog);
            return 0;

        case WM_DESTROY: {
            // 释放本对话框创建的字体（Stock 字体不归我们释放）。
            auto* state = reinterpret_cast<DialogState*>(
                GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (state != nullptr) {
                if (state->createdBodyFont != nullptr) {
                    DeleteObject(state->createdBodyFont);
                    state->createdBodyFont = nullptr;
                }
                if (state->createdTitleFont != nullptr) {
                    DeleteObject(state->createdTitleFont);
                    state->createdTitleFont = nullptr;
                }
            }
            // 唤醒模态循环去检查 IsWindow（绝不能 PostQuitMessage，
            // 会连带杀掉主消息循环 —— 项目早期踩过的坑）。
            HWND owner = GetWindow(dialog, GW_OWNER);
            if (owner != nullptr) {
                PostMessageW(owner, kDialogCloseMessage, 0, 0);
            }
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(dialog, message, wParam, lParam);
}

void ensureDialogClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &dialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"KTile.AboutDialog";

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

}  // namespace

void showAboutDialog(const HWND owner, const std::wstring& productName,
                     const std::wstring& version) {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetModuleHandleW(nullptr));
    ensureDialogClass(instance);

    DialogState state;
    state.productName = productName;
    state.version = version;

    // 居中于 owner；owner 为空时居中于主屏工作区。
    RECT baseRect{};
    if (owner != nullptr) {
        GetWindowRect(owner, &baseRect);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &baseRect, 0);
    }

    const double scale = scaleFor(owner != nullptr ? owner : GetDesktopWindow());
    const int width = scaled(scale, kClientWidth);
    const int height = scaled(scale, kClientHeight);
    const int x = baseRect.left +
                  ((baseRect.right - baseRect.left) - width) / 2;
    const int y = baseRect.top +
                  ((baseRect.bottom - baseRect.top) - height) / 2;

    RECT desired{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    const HWND dialog = CreateWindowExW(
        exStyle, L"KTile.AboutDialog", (infrastructure::i18n::trw("about.titlePrefix") + productName).c_str(), style,
        x, y, desired.right - desired.left, desired.bottom - desired.top,
        owner, nullptr, instance, &state);

    if (dialog == nullptr) {
        return;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环：与 text_input_dialog 同一套约定。
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kDialogCloseMessage) {
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
}

}  // namespace ui
}  // namespace ktile
