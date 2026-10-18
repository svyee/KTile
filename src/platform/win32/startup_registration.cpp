// KTile - Win32 平台层：开机自启动实现
#include "platform/win32/startup_registration.h"

#include <Windows.h>

#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

constexpr const wchar_t* kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// 读取注册表字符串值。返回 true 表示存在且读取成功。
bool readRunValue(const std::wstring& valueName, std::wstring* outValue) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key);
    if (openStatus != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD byteCount = 0;
    LSTATUS queryStatus = RegQueryValueExW(
        key, valueName.c_str(), nullptr, &type, nullptr, &byteCount);
    if (queryStatus != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)
        || byteCount == 0) {
        RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buffer(byteCount / sizeof(wchar_t) + 1, L'\0');
    queryStatus = RegQueryValueExW(
        key, valueName.c_str(), nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer.data()), &byteCount);

    RegCloseKey(key);

    if (queryStatus != ERROR_SUCCESS) {
        return false;
    }
    if (outValue != nullptr) {
        *outValue = std::wstring(buffer.data());
    }
    return true;
}

}  // namespace

bool StartupRegistration::isEnabled(const std::wstring& valueName) {
    std::wstring value;
    return readRunValue(valueName, &value);
}

Win32Status StartupRegistration::enable(const std::wstring& valueName,
                                       const std::wstring& executablePath) {
    std::wstring path = executablePath;
    if (path.empty()) {
        auto result = currentExecutablePath(&path);
        if (!result) {
            return base::makeUnexpected(result.error());
        }
    }

    HKEY key = nullptr;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (createStatus != ERROR_SUCCESS) {
        return base::makeUnexpected(Win32Error(
            static_cast<DWORD>(createStatus), "无法打开开机自启注册表项"));
    }

    // 路径含空格时加引号，避免被当作参数分隔
    std::wstring command = L"\"" + path + L"\"";

    const LSTATUS setStatus = RegSetValueExW(
        key,
        valueName.c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(key);

    if (setStatus != ERROR_SUCCESS) {
        return base::makeUnexpected(Win32Error(
            static_cast<DWORD>(setStatus), "写入开机自启注册表失败"));
    }
    return {};
}

Win32Status StartupRegistration::disable(const std::wstring& valueName) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key);
    if (openStatus != ERROR_SUCCESS) {
        // 键不存在等同于「未启用」，视为成功
        if (openStatus == ERROR_FILE_NOT_FOUND) {
            return {};
        }
        return base::makeUnexpected(Win32Error(
            static_cast<DWORD>(openStatus), "无法打开开机自启注册表项"));
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key, valueName.c_str());
    RegCloseKey(key);

    // 值不存在也算成功（目标状态已达成）
    if (deleteStatus != ERROR_SUCCESS && deleteStatus != ERROR_FILE_NOT_FOUND) {
        return base::makeUnexpected(Win32Error(
            static_cast<DWORD>(deleteStatus), "删除开机自启注册表值失败"));
    }
    return {};
}

Win32Status StartupRegistration::setEnabled(const std::wstring& valueName,
                                           const bool enabled) {
    return enabled ? enable(valueName) : disable(valueName);
}

Win32Status StartupRegistration::currentExecutablePath(std::wstring* outPath) {
    if (outPath == nullptr) {
        return base::makeUnexpected(
            Win32Error(ERROR_INVALID_PARAMETER, "输出参数为空"));
    }

    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return base::makeUnexpected(lastError("无法获取程序路径"));
        }
        // 缓冲区足够（未截断）：返回值小于缓冲区容量即表示完整路径
        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            *outPath = std::wstring(buffer.data(), length);
            return {};
        }
        // 路径超长，扩大缓冲区重试（上限 32768）
        if (buffer.size() >= 32768) {
            return base::makeUnexpected(
                Win32Error(ERROR_INSUFFICIENT_BUFFER, "程序路径过长"));
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
