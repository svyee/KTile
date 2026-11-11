#include "platform/win32/follow_launcher.h"

#include "platform/win32/program_paths.h"

#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <shlwapi.h>

#include <cwchar>
#include <cwctype>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 比对前统一形状：规范化 ".." / "."，让 "C:\a\..\b\x.exe" 与 "C:\b\x.exe"
// 相等。失败就退回原串（典型的畸形路径，不值得为它报错）。
std::wstring normalizeForCompare(const std::wstring& path) {
    if (path.empty()) {
        return path;
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    if (PathCanonicalizeW(buffer.data(), path.c_str()) != FALSE) {
        return std::wstring(buffer.data());
    }
    return path;
}

// 取路径里的文件名部分（用于「同名兜底」比较）。
std::wstring fileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// 小写扩展名（含点）。没有扩展名返回空串。
std::wstring lowercaseExtension(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && dot < slash) {
        return std::wstring();  // 点在目录名里（"C:\a.b\x"），不是扩展名
    }
    std::wstring extension = path.substr(dot);
    for (wchar_t& ch : extension) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return extension;
}

}  // namespace

std::wstring resolveShortcutTarget(const std::wstring& lnkPath) {
    if (lnkPath.empty()) {
        return std::wstring();
    }
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
        link == nullptr) {
        return std::wstring();
    }
    std::wstring target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) &&
        file != nullptr) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t path[32768] = {};
            // SLGP_RAWPATH 拿原始写法（可能带 %windir%），拿不到再按默认方式
            link->GetPath(path, 32768, nullptr, SLGP_RAWPATH);
            if (path[0] == L'\0') {
                link->GetPath(path, 32768, nullptr, 0);
            }
            if (path[0] != L'\0') {
                // 带环境变量的写法要展开，否则和进程映像路径永远对不上
                wchar_t expanded[32768] = {};
                if (ExpandEnvironmentStringsW(path, expanded, 32768) > 0 &&
                    expanded[0] != L'\0') {
                    target.assign(expanded);
                } else {
                    target.assign(path);
                }
            }
        }
        file->Release();
    }
    link->Release();
    return target;
}

bool isProgramRunning(const std::wstring& targetPath) {
    std::wstring target = resolveProgramRelativePath(targetPath);
    if (target.empty()) {
        return false;
    }

    const std::wstring extension = lowercaseExtension(target);
    if (extension == L".lnk") {
        const std::wstring resolved = resolveShortcutTarget(target);
        if (resolved.empty()) {
            return false;  // 解析不出来就只能照常启动，别拦着用户
        }
        target = resolved;
    } else if (extension != L".exe" && extension != L".com") {
        // 文档 / 文件夹 / 脚本：没有可靠的「已打开」判据
        return false;
    }

    const std::wstring wanted = normalizeForCompare(target);
    const std::wstring wantedName = fileNameOf(wanted);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<wchar_t> imagePath(32768, L'\0');
    bool running = false;

    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            // 空闲进程（PID 0）与系统进程打不开，直接跳过
            if (entry.th32ProcessID == 0) {
                continue;
            }
            // LIMITED_INFORMATION：Win7 起可用，普通权限就能查自己的
            // 大部分进程；查不到（系统进程 / 更高权限的）就当没这一条。
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            DWORD size = static_cast<DWORD>(imagePath.size());
            if (QueryFullProcessImageNameW(process, 0, imagePath.data(),
                                           &size) != FALSE) {
                const std::wstring image(imagePath.data(), size);
                if (_wcsicmp(normalizeForCompare(image).c_str(),
                             wanted.c_str()) == 0) {
                    running = true;
                } else if (_wcsicmp(fileNameOf(image).c_str(),
                                    wantedName.c_str()) == 0) {
                    // 同名兜底：完整路径对不上时按文件名再判一次。
                    //
                    // 为什么需要：商店应用（以及 System32 下那些「应用执行
                    // 别名」）启动后，真实进程的映像路径在 WindowsApps 里 ——
                    // 拿 C:\Windows\System32\notepad.exe 去比永远比不中，于是
                    // 每次启动都多开一个记事本。
                    //
                    // 代价是「同名不同路径」会被判成已运行（便携版同名程序）。
                    // 取舍很清楚：漏启动一次，用户点一下磁贴就补回来了；
                    // 而多开一路实例会留下实实在在的重复窗口，还可能有
                    // 两份托盘图标抢同一个配置。
                    running = true;
                }
            }
            CloseHandle(process);
        } while (!running && Process32NextW(snapshot, &entry) != FALSE);
    }

    CloseHandle(snapshot);
    return running;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
