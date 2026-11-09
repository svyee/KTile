#include "platform/win32/plugin_runner.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "base/text_convert.h"
#include "platform/win32/unique_handles.h"

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 简单但足够用的命令行引号处理：含空格/引号的参数整体加引号，
// 内部引号用 \" 转义。插件参数基本是路径和 URL，不会遇到极端转义规则。
std::wstring quoteArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    bool needsQuote = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuote) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (wchar_t ch : argument) {
        if (ch == L'"') {
            quoted += L"\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += L"\"";
    return quoted;
}

bool readTextFile(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 16LL * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        if (!ReadFile(file, &out[total],
                      static_cast<DWORD>(out.size() - total), &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            return false;
        }
        total += chunk;
    }
    CloseHandle(file);

    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// 继承当前进程环境，去掉旧的 KTILE_PLUGIN_CONFIG，再追加新的。
bool buildEnvironmentBlock(const std::string& configJson,
                           std::vector<wchar_t>* block) {
    std::vector<std::wstring> entries;

    wchar_t* environment = GetEnvironmentStringsW();
    if (environment == nullptr) {
        return false;
    }
    for (const wchar_t* cursor = environment; *cursor != L'\0';) {
        const std::wstring entry(cursor);
        if (entry.compare(0, 20, L"KTILE_PLUGIN_CONFIG=") != 0) {
            entries.push_back(entry);
        }
        cursor += entry.size() + 1;
    }
    FreeEnvironmentStringsW(environment);

    entries.push_back(L"KTILE_PLUGIN_CONFIG=" + base::utf8ToWide(configJson));
    std::sort(entries.begin(), entries.end());

    std::size_t total = 0;
    for (const std::wstring& entry : entries) {
        total += entry.size() + 1;
    }
    total += 1;

    block->assign(total, L'\0');
    std::size_t offset = 0;
    for (const std::wstring& entry : entries) {
        for (wchar_t ch : entry) {
            block->at(offset++) = ch;
        }
        block->at(offset++) = L'\0';
    }
    return true;
}

}  // namespace

PluginRawResult runCommandPlugin(
    const std::string& program,
    const std::vector<std::string>& arguments,
    const std::string& workingDirectory,
    const std::string& configJson,
    const std::string& outputFile,
    int timeoutSeconds) {
    PluginRawResult result;

    std::wstring wideProgram = base::utf8ToWide(program);
    if (wideProgram.empty()) {
        result.error = "program 无法编码为 UTF-16";
        return result;
    }
    const std::wstring wideDirectory = base::utf8ToWide(workingDirectory);

    // 没有路径分隔符时按插件目录解析，而不是依赖 PATH。
    if (wideProgram.find_first_of(L"\\/") == std::wstring::npos &&
        !wideDirectory.empty()) {
        wideProgram = wideDirectory + L"\\" + wideProgram;
    }

    std::wstring commandLine = quoteArgument(wideProgram);
    for (const std::string& argument : arguments) {
        commandLine += L" ";
        commandLine += quoteArgument(base::utf8ToWide(argument));
    }

    std::vector<wchar_t> environmentBlock;
    // 参数 JSON 为空时也传一个 {}，让脚本可以无条件读取。
    const std::string config = configJson.empty() ? "{}" : configJson;
    if (!buildEnvironmentBlock(config, &environmentBlock)) {
        result.error = "无法构造子进程环境变量";
        return result;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    UniqueHandle stdoutRead;
    UniqueHandle stdoutWrite;
    if (!CreatePipe(stdoutRead.put(), stdoutWrite.put(), &attributes, 0)) {
        result.error = "创建输出管道失败（错误码 " +
                       std::to_string(GetLastError()) + "）";
        return result;
    }
    // 读端不能被继承，否则子进程退出后管道仍保持打开。
    SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0);

    UniqueHandle nullInput;
    nullInput.reset(CreateFileW(L"NUL", GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    UniqueHandle nullError;
    nullError.reset(CreateFileW(L"NUL", GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.valid() ? nullInput.get()
                                          : stdoutRead.get();
    startup.hStdOutput = stdoutWrite.get();
    startup.hStdError = nullError.valid() ? nullError.get() : stdoutWrite.get();

    PROCESS_INFORMATION processInfo{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                        CREATE_SUSPENDED;
    if (!CreateProcessW(wideProgram.c_str(), commandLine.data(), nullptr,
                        nullptr, TRUE, flags,
                        environmentBlock.empty() ? nullptr
                                                 : environmentBlock.data(),
                        wideDirectory.empty() ? nullptr : wideDirectory.c_str(),
                        &startup, &processInfo)) {
        result.error = "启动命令失败（错误码 " +
                       std::to_string(GetLastError()) + "）";
        return result;
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle thread(processInfo.hThread);
    UniqueHandle job;
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (job.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                &info, sizeof(info));
        // Win7 下若当前进程已在 Job 中，这里可能失败；不影响基本执行。
        AssignProcessToJobObject(job.get(), process.get());
    }

    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        result.error = "恢复子进程失败";
        return result;
    }

    // 父进程必须关闭写端，否则读循环无法通过 EOF 结束。
    stdoutWrite.reset();

    const ULONGLONG started = GetTickCount64();
    const ULONGLONG timeoutMs =
        static_cast<ULONGLONG>(timeoutSeconds > 0 ? timeoutSeconds : 8) * 1000ULL;

    std::string output;
    bool timedOut = false;
    bool tooLarge = false;

    for (;;) {
        DWORD available = 0;
        const bool peeked =
            PeekNamedPipe(stdoutRead.get(), nullptr, 0, nullptr, &available, nullptr) != FALSE;
        if (peeked && available > 0) {
            char buffer[4096];
            const DWORD want = available < sizeof(buffer)
                                   ? available
                                   : static_cast<DWORD>(sizeof(buffer));
            DWORD read = 0;
            if (ReadFile(stdoutRead.get(), buffer, want, &read, nullptr) &&
                read > 0) {
                output.append(buffer, read);
                if (output.size() > 8ULL * 1024 * 1024) {
                    tooLarge = true;
                    break;
                }
                continue;
            }
        }

        if (WaitForSingleObject(process.get(), 20) == WAIT_OBJECT_0) {
            // 进程结束后把管道里剩余数据读完再退出。
            DWORD remaining = 0;
            if (PeekNamedPipe(stdoutRead.get(), nullptr, 0, nullptr, &remaining,
                              nullptr) &&
                remaining > 0) {
                continue;
            }
            break;
        }

        if (GetTickCount64() - started > timeoutMs) {
            timedOut = true;
            if (job.valid()) {
                TerminateJobObject(job.get(), 1);
            } else {
                TerminateProcess(process.get(), 1);
            }
            break;
        }
    }

    if (tooLarge) {
        result.error = "命令输出超过 8MB，已中止";
        return result;
    }
    if (timedOut) {
        result.error = "命令执行超时（" +
                       std::to_string(timeoutSeconds) + " 秒）";
        return result;
    }

    WaitForSingleObject(process.get(), 3000);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.get(), &exitCode);

    if (output.empty() && !outputFile.empty()) {
        const std::wstring filePath =
            (wideDirectory.empty() ? std::wstring() : wideDirectory + L"\\") +
            base::utf8ToWide(outputFile);
        std::string fileText;
        if (readTextFile(filePath, fileText)) {
            output = std::move(fileText);
        }
    }

    if (output.empty()) {
        result.error = exitCode == 0
                           ? "命令没有输出 JSON"
                           : "命令以退出码 " + std::to_string(exitCode) +
                                 " 结束且没有输出";
        return result;
    }

    result.body = std::move(output);
    result.ok = true;
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
