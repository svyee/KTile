// KTile - UI 层：「关于」对话框实现
#include "ui/about_dialog.h"

#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// 客户区尺寸（DIP）。
constexpr int kClientWidth = 380;
constexpr int kClientHeight = 240;

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

// 控件 ID。确定按钮用 IDOK：回车（IsDialogMessageW）与点击同 ID，
// Esc 发 IDCANCEL —— 对纯展示对话框两者都等于「关闭」。
constexpr int kTitleId = 2001;
constexpr int kVersionId = 2002;
constexpr int kDescId = 2003;
constexpr int kCopyrightId = 2004;
constexpr int kSeparatorId = 2005;
constexpr int kEmailId = 2006;
constexpr int kWechatId = 2007;

// 对话框关闭通知（结束本地模态循环）。与 text_input_dialog 的
// WM_APP+0x93 区分开，避免两条模态链路互相唤醒看不懂。
constexpr UINT kDialogCloseMessage = WM_APP + 0x94U;

// exe 内嵌的图标资源 ID（与 app.rc 的 101 ICON 对应）。
constexpr int kAppIconResourceId = 101;

struct DialogState {
    std::wstring productName;
    std::wstring version;
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

void finishDialog(const HWND dialog) {
    DestroyWindow(dialog);
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

        case WM_CREATE: {
            auto* state = reinterpret_cast<DialogState*>(
                GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (state == nullptr) {
                return -1;
            }

            const double scale = scaleFor(dialog);
            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            const HFONT bodyFont = static_cast<HFONT>(
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

            // 标题字体：正文沿用 DEFAULT_GUI_FONT，标题单做一个
            // 加粗大字（微软雅黑 UI，Win7 SP1 起自带）。
            const HDC dc = GetDC(dialog);
            const int dpiY = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
            if (dc != nullptr) {
                ReleaseDC(dialog, dc);
            }
            const HFONT titleFont = CreateFontW(
                -MulDiv(14, dpiY > 0 ? dpiY : 96, 72), 0, 0, 0, FW_BOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                L"Microsoft YaHei UI");

            // 版本行先拼成具名 wstring：数组里只能存 c_str()，
            // 直接内联临时对象会在表达式结束时就析构（悬垂）。
            const std::wstring versionLine = L"版本 " + state->version;

            struct LineSpec {
                int id;
                int y;
                const wchar_t* text;
            };
            const LineSpec lines[] = {
                {kVersionId, kVersionY, versionLine.c_str()},
                {kDescId, kDescY, L"轻量 Windows 磁贴式应用启动器"},
                {kCopyrightId, kCopyrightY, L"© 2026 svyee · 奇竹科技"},
                {kEmailId, kEmailY, L"邮箱：svyee@163.com"},
                {kWechatId, kWechatY, L"微信：svyeee"},
            };

            std::vector<HWND> children;
            children.reserve(9);

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

            const HWND ok = CreateWindowExW(
                0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
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
            }
            return 0;

        case WM_CLOSE:
            finishDialog(dialog);
            return 0;

        case WM_DESTROY: {
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
        exStyle, L"KTile.AboutDialog", (L"关于 " + productName).c_str(), style,
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
