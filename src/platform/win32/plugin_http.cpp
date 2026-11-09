#include "platform/win32/plugin_http.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>

#include "base/text_convert.h"

namespace ktile {
namespace platform {
namespace win32 {
namespace {

struct InternetHandle {
    HINTERNET handle = nullptr;

    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    ~InternetHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
};

std::string winHttpErrorText(const char* stage) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "（%s，错误码 %lu）", stage,
                  static_cast<unsigned long>(GetLastError()));
    return std::string(buffer);
}

}  // namespace

PluginRawResult httpRequestPlugin(
    const std::string& url,
    const std::string& method,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    int timeoutSeconds) {
    PluginRawResult result;

    const std::wstring wideUrl = base::utf8ToWide(url);
    if (wideUrl.empty()) {
        result.error = "URL 无法编码为 UTF-16";
        return result;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        result.error = "URL 格式无效" + winHttpErrorText("CrackUrl");
        return result;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo != nullptr) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    InternetHandle session;
    session.handle = WinHttpOpen(
        L"KTile/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.handle == nullptr) {
        result.error = "初始化 WinHTTP 失败" + winHttpErrorText("Open");
        return result;
    }

    const int timeoutMs = timeoutSeconds > 0 ? timeoutSeconds * 1000 : 8000;
    WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    InternetHandle connection;
    connection.handle =
        WinHttpConnect(session.handle, host.c_str(),
                       static_cast<INTERNET_PORT>(components.nPort), 0);
    if (connection.handle == nullptr) {
        result.error = "连接主机失败" + winHttpErrorText("Connect");
        return result;
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring wideMethod = base::utf8ToWide(method.empty() ? "GET" : method);
    InternetHandle request;
    request.handle = WinHttpOpenRequest(
        connection.handle, wideMethod.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, nullptr,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (request.handle == nullptr) {
        result.error = "创建请求失败" + winHttpErrorText("OpenRequest");
        return result;
    }

    std::wstring headerText;
    for (const auto& header : headers) {
        headerText += base::utf8ToWide(header.first);
        headerText += L": ";
        headerText += base::utf8ToWide(header.second);
        headerText += L"\r\n";
    }

    const bool sendBody = !body.empty() &&
                          (wideMethod == L"POST" || wideMethod == L"PUT" ||
                           wideMethod == L"PATCH");

    if (!WinHttpSendRequest(
            request.handle,
            headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
            headerText.empty() ? 0 : static_cast<DWORD>(headerText.size()),
            sendBody ? const_cast<char*>(body.c_str()) : nullptr,
            sendBody ? static_cast<DWORD>(body.size()) : 0,
            0, 0)) {
        result.error = "发送请求失败" + winHttpErrorText("SendRequest");
        return result;
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        result.error = "接收响应失败" + winHttpErrorText("ReceiveResponse");
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(
            request.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode >= 400) {
            result.error = "服务器返回 HTTP " + std::to_string(statusCode);
            return result;
        }
    }

    std::string response;
    response.reserve(4096);
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer,
                             static_cast<DWORD>(sizeof(buffer)), &read)) {
            result.error = "读取响应失败" + winHttpErrorText("ReadData");
            return result;
        }
        if (read == 0) {
            break;
        }
        response.append(buffer, read);
        if (response.size() > 8ULL * 1024 * 1024) {
            result.error = "响应体超过 8MB，已中止";
            return result;
        }
    }

    if (response.empty()) {
        result.error = "服务器返回空响应";
        return result;
    }

    result.body = std::move(response);
    result.ok = true;
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
