#include "platform/win32/item_launcher.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "platform/win32/system_actions.h"

#include <shellapi.h>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 把一条参数包成可追加到命令行的形式。
//
// 用户在「参数」里每行填一条；含空格的参数必须整体加引号，
// 否则会被目标程序拆成多个参数（比如 "C:\Program Files\x" 之类）。
// 已带引号的不重复包。
std::wstring quoteArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.front() == L'"' && argument.back() == L'"' &&
        argument.size() >= 2) {
        return argument;  // 用户自己写了引号，尊重原样
    }
    if (argument.find_first_of(L" \t") == std::wstring::npos) {
        return argument;  // 不含空白，不必加引号
    }
    std::wstring result = L"\"";
    result += argument;
    result += L'"';
    return result;
}

// 拼出 ShellExecuteW 的 lpParameters（UTF-16）。
std::wstring buildParameterString(const core::LaunchItem& item) {
    std::wstring parameters;
    for (const std::string& raw : item.arguments) {
        std::wstring argument = base::utf8ToWideLossy(raw);
        if (argument.empty()) {
            continue;
        }
        if (!parameters.empty()) {
            parameters += L' ';
        }
        parameters += quoteArgument(argument);
    }
    return parameters;
}

// 工作目录在 launchItem 里转成局部 std::wstring 后取 c_str() 传参 ——
// ShellExecuteW 同步返回（它只负责拉起进程），调用期间字符串有效。

std::wstring lastErrorMessage(const HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD copied = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message;
    if (copied > 0 && buffer != nullptr) {
        message.assign(buffer, copied);
        LocalFree(buffer);
    }
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    if (message.empty()) {
        message = L"未知错误 (0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L")";
    }
    return message;
}

}  // namespace

LaunchResult launchItem(const core::LaunchItem& item,
                        const bool forceAdministrator) {
    const std::wstring target = base::utf8ToWideLossy(item.target);
    if (target.empty()) {
        return {false, L"启动目标为空。"};
    }

    // 内置命令（"ktile:<id>"）没有文件或协议可执行，在 ShellExecute
    // 之前分派到系统命令执行端。提权参数对内置命令无意义 —— 命令自己
    // 知道要不要特权（如关机需要 SE_SHUTDOWN_NAME，内部已处理）。
    const std::wstring builtinId = core::builtinIdFromTarget(target);
    if (!builtinId.empty()) {
        return runBuiltinAction(builtinId);
    }

    const std::wstring parameters = buildParameterString(item);
    const std::wstring workingDirectory =
        (item.workingDirectory.has_value() && !item.workingDirectory->empty())
            ? base::utf8ToWideLossy(*item.workingDirectory)
            : std::wstring();

    // 动词：提权用 runas（触发 UAC）；否则 open 交给系统按类型分流。
    // runAsAdministrator 与 forceAdministrator 任一为真都提权 ——
    // 右键菜单的「以管理员身份运行」应无视项目自身设置。
    const bool elevate =
        forceAdministrator || item.runAsAdministrator;
    const wchar_t* verb = elevate ? L"runas" : L"open";

    const HINSTANCE instance = ShellExecuteW(
        nullptr,
        verb,
        target.c_str(),
        parameters.empty() ? nullptr : parameters.c_str(),
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        SW_SHOWNORMAL);

    // ShellExecuteW 返回值 > 32 表示成功；否则是错误码
    // （SE_ERR_* 常量与 Win32 错误码共用空间，直接按 int 解释）。
    const intptr_t code = reinterpret_cast<intptr_t>(instance);
    if (code > 32) {
        return {true, std::wstring()};
    }

    std::wstring error = L"无法启动「" + target + L"」";
    if (elevate && code == SE_ERR_ACCESSDENIED) {
        error += L"：\n用户在 UAC 提示中取消了提权。";
    } else {
        error += L"：\n" + lastErrorMessage(static_cast<HRESULT>(code));
    }
    return {false, std::move(error)};
}

LaunchResult revealItemInExplorer(const core::LaunchItem& item) {
    // 内置命令与网址一样没有「文件位置」；类型检查主要靠调用方禁用
    // 菜单项，这里再兜底一次，防止误调用。
    if (item.type == core::ItemType::Url ||
        !core::builtinIdFromTarget(base::utf8ToWideLossy(item.target)).empty()) {
        return {false, L"该项目没有文件位置。"};
    }

    const std::wstring target = base::utf8ToWideLossy(item.target);
    if (target.empty()) {
        return {false, L"启动目标为空。"};
    }

    // explorer /select,"路径" —— 打开所在文件夹并高亮该项；
    // 目标本身是文件夹时同样适用（高亮该文件夹本身）。
    std::wstring arguments = L"/select,\"";
    arguments += target;
    arguments += L'"';

    const HINSTANCE instance = ShellExecuteW(
        nullptr, L"open", L"explorer.exe", arguments.c_str(),
        nullptr, SW_SHOWNORMAL);

    const intptr_t code = reinterpret_cast<intptr_t>(instance);
    if (code > 32) {
        return {true, std::wstring()};
    }
    return {false, L"无法打开文件位置：\n" +
                       lastErrorMessage(static_cast<HRESULT>(code))};
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
