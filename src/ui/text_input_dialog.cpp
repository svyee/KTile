// KTile - UI 层：单行文本输入对话框实现
#include "ui/text_input_dialog.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// 客户区尺寸（DIP）。够放下一行说明 + 一个 20 多字的输入框。
constexpr int kClientWidth = 340;
constexpr int kClientHeight = 116;

constexpr int kMargin = 16;
constexpr int kLabelY = 14;
constexpr int kLabelHeight = 20;
constexpr int kEditY = 38;
constexpr int kEditHeight = 24;
constexpr int kButtonWidth = 84;
constexpr int kButtonHeight = 28;
constexpr int kButtonGap = 8;
constexpr int kButtonY = 74;

constexpr int kPromptId = 2001;
constexpr int kEditId = 2002;
// 确定/取消直接用 IDOK / IDCANCEL：IsDialogMessageW 处理回车/Esc 时
// 发送的就是这两个 ID，按钮点击也发同 ID，一套代码两处来源。
// （与项目编辑对话框同一约定。）

constexpr const wchar_t* kDialogClass = L"KTile.TextInputDialog";

// 对话框关闭通知（结束本地模态循环，不影响主消息循环）。
// 绝不能用 PostQuitMessage：它投递的是线程级粘性 WM_QUIT，
// 会连带杀掉主窗口的消息循环 —— 这是本项目踩过的坑。
constexpr UINT kDialogCloseMessage = WM_APP + 0x93U;

struct DialogState {
    std::wstring prompt;
    std::wstring initialText;
    std::wstring result;
    bool accepted = false;
};

// 把 owner 的 DPI 换算成缩放系数（96 DPI = 1.0）。
//
// 不用 GetDpiForWindow：那是 Win10 1607+ 的 API，本项目要保持
// Win7 SP1 可运行。GetDeviceCaps 全平台可用，精度对本对话框足够。
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

// 裁掉首尾空白（含全角空格 U+3000，用户在中文输入法下很容易打出来）。
std::wstring trimWide(const std::wstring& value) {
    auto isSpace = [](const wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' ||
               ch == L'\v' || ch == L'\f' || ch == 0x3000 || ch == 0x00A0;
    };
    std::size_t begin = 0;
    while (begin < value.size() && isSpace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && isSpace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

void finishDialog(const HWND dialog, const bool accepted) {
    auto* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (state != nullptr) {
        state->accepted = accepted;
    }
    DestroyWindow(dialog);
}

LRESULT CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            // 注意这里是 break 而不是 return TRUE：标题栏文字由
            // DefWindowProcW 从 CREATESTRUCT 里存进去，提前 return 会让
            // 标题一直空着（本项目在项目编辑对话框上踩过这个坑）。
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
            const HFONT font = static_cast<HFONT>(
                GetStockObject(DEFAULT_GUI_FONT));

            const int labelY = scaled(scale, kLabelY);
            const int editY = scaled(scale, kEditY);
            const int editHeight = scaled(scale, kEditHeight);
            const int buttonY = scaled(scale, kButtonY);
            const int buttonWidth = scaled(scale, kButtonWidth);
            const int buttonHeight = scaled(scale, kButtonHeight);
            const int gap = scaled(scale, kButtonGap);
            const int clientWidth = scaled(scale, kClientWidth);
            const int margin = scaled(scale, kMargin);
            const int fieldWidth = clientWidth - margin * 2;

            const HWND prompt = CreateWindowExW(
                0, L"STATIC", state->prompt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                margin, labelY, fieldWidth, scaled(scale, kLabelHeight),
                dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptId)),
                instance, nullptr);

            const HWND edit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", state->initialText.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                margin, editY, fieldWidth, editHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                instance, nullptr);

            const int cancelX = clientWidth - margin - buttonWidth;
            const int okX = cancelX - gap - buttonWidth;

            const HWND ok = CreateWindowExW(
                0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                okX, buttonY, buttonWidth, buttonHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                instance, nullptr);

            const HWND cancel = CreateWindowExW(
                0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                cancelX, buttonY, buttonWidth, buttonHeight, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                instance, nullptr);

            if (prompt == nullptr || edit == nullptr || ok == nullptr ||
                cancel == nullptr) {
                return -1;
            }

            for (const HWND child : {prompt, edit, ok, cancel}) {
                SendMessageW(child, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }

            // 打开即全选：重命名时直接打字就替换掉原名字，
            // 想改一个字也不必先手动全选。
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK: {
                    auto* state = reinterpret_cast<DialogState*>(
                        GetWindowLongPtrW(dialog, GWLP_USERDATA));
                    if (state == nullptr) {
                        return 0;
                    }
                    wchar_t buffer[512] = {};
                    GetDlgItemTextW(dialog, kEditId, buffer,
                                    static_cast<int>(std::size(buffer)));
                    const std::wstring trimmed = trimWide(buffer);
                    if (trimmed.empty()) {
                        // 空名字必须挡在这里：标签条上一片点不中的空白
                        // 比「多按一次确定」糟糕得多。
                        MessageBoxW(dialog, L"名字不能为空。", L"KTile",
                                    MB_OK | MB_ICONINFORMATION);
                        SetFocus(GetDlgItem(dialog, kEditId));
                        return 0;
                    }
                    state->result = trimmed;
                    finishDialog(dialog, true);
                    return 0;
                }
                case IDCANCEL:
                    finishDialog(dialog, false);
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_CLOSE:
            finishDialog(dialog, false);
            return 0;

        case WM_DESTROY: {
            // 唤醒模态循环去检查 IsWindow：只要窗口没了循环就退出。
            // 自己的消息窗口收不到时也没有关系 —— 循环里 GetMessage
            // 还会被主窗口的其它消息唤醒。
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
    windowClass.lpszClassName = kDialogClass;

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

}  // namespace

bool showTextInputDialog(const HWND owner, const std::wstring& title,
                         const std::wstring& prompt,
                         const std::wstring& initialText,
                         std::wstring* outText) {
    if (outText == nullptr || owner == nullptr) {
        return false;
    }

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    ensureDialogClass(instance);

    DialogState state;
    state.prompt = prompt;
    state.initialText = initialText;

    // 居中于 owner（与项目编辑对话框同一算法）
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const double scale = scaleFor(owner);
    const int width = scaled(scale, kClientWidth);
    const int height = scaled(scale, kClientHeight);
    const int x =
        ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y =
        ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    RECT desired{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    const HWND dialog = CreateWindowExW(
        exStyle, kDialogClass, title.c_str(), style, x, y,
        desired.right - desired.left, desired.bottom - desired.top, owner,
        nullptr, instance, &state);

    if (dialog == nullptr) {
        return false;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    // 本地模态循环：退出条件是 IsWindow(dialog)，不依赖 WM_QUIT。
    // 属于对话框的消息走 IsDialogMessageW（Tab 切换 / 回车 = 确定 /
    // Esc = 取消），其余消息照常派发 —— 托盘与热键在对话框打开期间仍然可用。
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

    if (!state.accepted) {
        return false;
    }
    *outText = state.result;
    return true;
}

}  // namespace ui
}  // namespace ktile
