#include "platform/win32/item_launcher.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "infrastructure/i18n.h"
#include "platform/win32/program_paths.h"
#include "platform/win32/system_actions.h"

#include <shellapi.h>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 取词（宽字符拷贝）。
//
// 语言表里的值存在字典里，指针在下次切换语言前一直有效；但这边的
// 结果会被 return 出去，所以立刻拷成 std::wstring，不留指针。
std::wstring W(const char* key) {
    return std::wstring(infrastructure::i18n::trw(key));
}

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
        message = infrastructure::i18n::formatW(
            "error.unknownError",
            {std::to_wstring(static_cast<unsigned long>(hr))});
    }
    return message;
}

}  // namespace

LaunchResult launchItem(const core::LaunchItem& item,
                        const bool forceAdministrator) {
    const std::wstring rawTarget = base::utf8ToWideLossy(item.target);
    if (rawTarget.empty()) {
        return {false, W("error.launchEmptyTarget")};
    }

    // 内置命令（"ktile:<id>"）没有文件或协议可执行，在 ShellExecute
    // 之前分派到系统命令执行端。提权参数对内置命令无意义 —— 命令自己
    // 知道要不要特权（如关机需要 SE_SHUTDOWN_NAME，内部已处理）。
    const std::wstring builtinId = core::builtinIdFromTarget(rawTarget);
    if (!builtinId.empty()) {
        return runBuiltinAction(builtinId);
    }

    // 相对路径以 exe 目录为基准（"tools\app.exe"），绝对路径 / URL /
    // 裸命令名原样通过，见 program_paths.h。
    const std::wstring target = resolveProgramRelativePath(rawTarget);

    const std::wstring parameters = buildParameterString(item);
    const std::wstring workingDirectory =
        (item.workingDirectory.has_value() && !item.workingDirectory->empty())
            ? resolveProgramRelativePath(
                  base::utf8ToWideLossy(*item.workingDirectory))
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

    std::wstring error = infrastructure::i18n::formatW("error.launchFailed", {target});
    if (elevate && code == SE_ERR_ACCESSDENIED) {
        error += W("error.uacCancelled");
    } else {
        error += lastErrorMessage(static_cast<HRESULT>(code));
    }
    return {false, std::move(error)};
}

LaunchResult revealItemInExplorer(const core::LaunchItem& item) {
    // 内置命令与网址一样没有「文件位置」；类型检查主要靠调用方禁用
    // 菜单项，这里再兜底一次，防止误调用。
    if (item.type == core::ItemType::Url ||
        !core::builtinIdFromTarget(base::utf8ToWideLossy(item.target)).empty()) {
        return {false, W("error.noFileLocation")};
    }

    const std::wstring target =
        resolveProgramRelativePath(base::utf8ToWideLossy(item.target));
    if (target.empty()) {
        return {false, W("error.launchEmptyTarget")};
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
    return {false, W("error.openLocationFailed") +
                       lastErrorMessage(static_cast<HRESULT>(code))};
}

LaunchResult openPathWithDefaultProgram(const std::wstring& path) {
    if (path.empty()) {
        return {false, W("error.pathEmpty")};
    }

    HINSTANCE instance = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                                       nullptr, SW_SHOWNORMAL);
    intptr_t code = reinterpret_cast<intptr_t>(instance);
    if (code > 32) {
        return {true, std::wstring()};
    }

    // 没有文件关联：记事本兜底。
    //
    // 只在「没有关联」这类错误上兜底，其它失败（文件被删、权限不足）记事本
    // 同样打不开，硬试一次只会把原始错误码盖成一个更含糊的报错。
    if (code == SE_ERR_NOASSOC || code == SE_ERR_ASSOCINCOMPLETE ||
        code == SE_ERR_DDEFAIL) {
        const std::wstring quoted = L"\"" + path + L"\"";
        // notepad.exe 交给 PATH 解析：System32 一定在 PATH 里，
        // 而写死 C:\Windows 在系统装在别的盘符时会失效。
        instance = ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                 quoted.c_str(), nullptr, SW_SHOWNORMAL);
        code = reinterpret_cast<intptr_t>(instance);
        if (code > 32) {
            return {true, std::wstring()};
        }
    }
    return {false, W("error.openFileFailed") +
                       lastErrorMessage(static_cast<HRESULT>(code))};
}

LaunchResult openFolderInExplorer(const std::wstring& path) {
    if (path.empty()) {
        return {false, W("error.pathEmpty")};
    }
    const HINSTANCE instance = ShellExecuteW(nullptr, L"open", path.c_str(),
                                             nullptr, nullptr, SW_SHOWNORMAL);
    const intptr_t code = reinterpret_cast<intptr_t>(instance);
    if (code > 32) {
        return {true, std::wstring()};
    }
    return {false, W("error.openFolderFailed") +
                       lastErrorMessage(static_cast<HRESULT>(code))};
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
