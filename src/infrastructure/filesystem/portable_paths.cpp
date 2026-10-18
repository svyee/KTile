// KTile - 基础设施层：便携路径解析实现
#include "infrastructure/filesystem/portable_paths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <vector>

namespace ktile {
namespace infrastructure {
namespace {

// 取当前 exe 的完整路径（自增长缓冲，避免 MAX_PATH 截断）
bool currentExecutablePath(std::wstring& out) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return false;
        }
        if (length < buffer.size() - 1) {
            out.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() > 32768) {
            return false;  // 病态情况，放弃
        }
        buffer.resize(buffer.size() * 2);
    }
}

// 去掉路径末尾的文件名，保留目录（结果不带结尾反斜杠）
std::wstring directoryOf(const std::wstring& filePath) {
    const std::size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return filePath.substr(0, slash);
}

// 探测目录是否可写：写一个临时文件再删除
bool isDirectoryWritable(const std::wstring& directory) {
    const std::wstring probe = directory + L"\\.ktile_write_probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    return true;
}

// 递归创建目录
bool createDirectoryRecursive(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // 逐级向上创建父目录
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {  // 不越过 "C:\"
        const std::wstring parent = path.substr(0, slash);
        if (!createDirectoryRecursive(parent)) {
            return false;
        }
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    // 并发创建时可能已被他人建好
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// 取 %LOCALAPPDATA% 下的降级根目录
bool fallbackRootDirectory(std::wstring& out) {
    PWSTR localAppData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData,
                                            0, nullptr, &localAppData);
    if (FAILED(hr) || localAppData == nullptr) {
        return false;
    }
    out.assign(localAppData);
    out += L"\\KTile";
    CoTaskMemFree(localAppData);
    return true;
}

}  // namespace

bool PortablePaths::initialize() {
    std::wstring exePath;
    if (!currentExecutablePath(exePath)) {
        return false;
    }

    executableDirectory_ = directoryOf(exePath);
    if (executableDirectory_.empty()) {
        return false;
    }

    // 优先使用 exe 同级的 data 目录（绿色模式）
    const std::wstring candidateData = executableDirectory_ + L"\\data";
    if (isDirectoryWritable(executableDirectory_) ||
        GetFileAttributesW(candidateData.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (createDirectoryRecursive(candidateData)) {
            portable_ = true;
            dataDirectory_ = candidateData;
            return true;
        }
    }

    // 降级：%LOCALAPPDATA%\KTile\data
    std::wstring fallbackRoot;
    if (!fallbackRootDirectory(fallbackRoot)) {
        return false;
    }
    const std::wstring fallbackData = fallbackRoot + L"\\data";
    if (!createDirectoryRecursive(fallbackData)) {
        return false;
    }

    portable_ = false;
    dataDirectory_ = fallbackData;
    return true;
}

std::wstring PortablePaths::itemsFilePath() const {
    return dataDirectory_ + L"\\items.json";
}

std::wstring PortablePaths::configFilePath() const {
    return dataDirectory_ + L"\\config.json";
}

std::wstring PortablePaths::iconsDirectory() const {
    return dataDirectory_ + L"\\icons";
}

std::wstring PortablePaths::logFilePath() const {
    return dataDirectory_ + L"\\ktile.log";
}

bool PortablePaths::ensureDirectoryExists(const std::wstring& path) const {
    return createDirectoryRecursive(path);
}

}  // namespace infrastructure
}  // namespace ktile
