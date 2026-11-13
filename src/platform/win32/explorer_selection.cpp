// KTile - Win32 平台层：explorer / 桌面文件选中状态查询实现
#include "platform/win32/explorer_selection.h"

// build.ps1 定义 WIN32_LEAN_AND_MEAN，windows.h 不带出 ole2.h：ExDisp.h
// 依赖的 interface 宏在 objbase.h 里，必须显式引入且排在 ExDisp 之前。
#include <objbase.h>
#include <ExDisp.h>
#include <ShlDisp.h>

#include <cwctype>

namespace ktile {
namespace platform {
namespace win32 {

namespace {

// COM 资源按查询失败路径一路释放，不用智能指针：涉及到的接口数量少，
// 失败分支都是直接返回，手工 Release 反而少一层依赖。
//
// 查询一次的开销：CoCreateInstance + 枚举 + 一次跨进程 Document 调用，
// 常态几毫秒，只在轮盘弹出瞬间发生，不值得为它常驻 COM 初始化。

// 窗口类名转小写，与 WindowTarget 的字段口径一致。
std::wstring classLower(HWND window) {
    wchar_t name[256] = {};
    if (GetClassNameW(window, name, 256) == 0) {
        return std::wstring();
    }
    std::wstring result = name;
    for (wchar_t& ch : result) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return result;
}

// IShellWindows 枚举项对应的顶层窗口是否就是目标窗口。多标签下每个标签
// 有自己的宿主 HWND，GA_ROOT 后才等于框架窗口，两种都算命中。
bool windowMatchesItem(HWND target, HWND itemWindow) {
    if (itemWindow == nullptr) {
        return false;
    }
    if (itemWindow == target) {
        return true;
    }
    return GetAncestor(itemWindow, GA_ROOT) == target;
}

// 从一个 shell 视图浏览器对象取选中文件数。Document 是跨进程的
// IDispatch，QueryInterface 到 IShellFolderViewDual 后走 SelectedItems。
// 视图刚切换 / 文件夹正在加载时 COM 调用可能失败，返回 -1 表示未知。
long selectedCountForItem(IWebBrowser2* browser) {
    if (browser == nullptr) {
        return -1;
    }
    IDispatch* document = nullptr;
    if (browser->get_Document(&document) != S_OK || document == nullptr) {
        return -1;
    }

    long count = -1;
    IShellFolderViewDual* view = nullptr;
    if (document->QueryInterface(IID_PPV_ARGS(&view)) == S_OK && view != nullptr) {
        FolderItems* items = nullptr;
        if (view->SelectedItems(&items) == S_OK && items != nullptr) {
            long selected = 0;
            if (items->get_Count(&selected) == S_OK) {
                count = selected;
            }
            items->Release();
        }
        view->Release();
    }
    document->Release();
    return count;
}

}  // namespace

bool isShellSelectionTarget(const std::wstring& processName,
                            const std::wstring& windowClass) {
    if (processName != L"explorer.exe") {
        return false;
    }
    return windowClass == L"cabinetwclass" ||
           windowClass == L"progman" || windowClass == L"workerw";
}

std::optional<bool> shellWindowHasSelection(HWND targetWindow) {
    if (targetWindow == nullptr) {
        return std::nullopt;
    }
    const std::wstring windowClass = classLower(targetWindow);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return std::nullopt;
    }

    std::optional<bool> result;
    IShellWindows* windows = nullptr;
    if (CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&windows)) == S_OK && windows != nullptr) {
        if (windowClass == L"progman" || windowClass == L"workerw") {
            // 桌面视图不在常规枚举里按固定顺序出现（枚举首项往往是最近
            // 打开的资源管理器窗口），必须走官方的 FindWindowSW(SWC_DESKTOP)：
            // 无论桌面宿主是 Progman 还是 WorkerW，它都返回桌面视图本身。
            VARIANT empty;
            VariantInit(&empty);
            VARIANT emptyRoot;
            VariantInit(&emptyRoot);
            long desktopWindow = 0;
            IDispatch* item = nullptr;
            if (windows->FindWindowSW(&empty, &emptyRoot, SWC_DESKTOP,
                                      &desktopWindow, SWFO_NEEDDISPATCH,
                                      &item) == S_OK && item != nullptr) {
                IWebBrowser2* browser = nullptr;
                if (item->QueryInterface(IID_PPV_ARGS(&browser)) == S_OK) {
                    const long count = selectedCountForItem(browser);
                    if (count >= 0) {
                        result = count > 0;
                    }
                    browser->Release();
                }
                item->Release();
            }
        } else {
            // 文件窗口：枚举全部 shell 视图，匹配目标窗口。多标签时同一
            // 框架有多个条目，只认可见的（后台标签的旧选中不算数）。
            IUnknown* enumerator = nullptr;
            if (windows->_NewEnum(&enumerator) == S_OK && enumerator != nullptr) {
                IEnumVARIANT* iter = nullptr;
                if (enumerator->QueryInterface(IID_PPV_ARGS(&iter)) == S_OK &&
                    iter != nullptr) {
                    bool matchedView = false;
                    VARIANT element;
                    while (iter->Next(1, &element, nullptr) == S_OK) {
                        if (element.vt == VT_DISPATCH && element.pdispVal != nullptr) {
                            IWebBrowser2* browser = nullptr;
                            if (element.pdispVal->QueryInterface(
                                    IID_PPV_ARGS(&browser)) == S_OK &&
                                browser != nullptr) {
                                SHANDLE_PTR itemHandle = 0;
                                browser->get_HWND(&itemHandle);
                                HWND itemWindow = reinterpret_cast<HWND>(
                                    static_cast<LONG_PTR>(itemHandle));
                                if (windowMatchesItem(targetWindow, itemWindow) &&
                                    IsWindowVisible(itemWindow) != FALSE) {
                                    matchedView = true;
                                    const long count = selectedCountForItem(browser);
                                    if (count > 0) {
                                        result = true;
                                    } else if (count == 0 && !result.has_value()) {
                                        result = false;
                                    }
                                }
                                browser->Release();
                            }
                        }
                        VariantClear(&element);
                        if (result == true) {
                            break;  // 有选中即定论，不必扫完
                        }
                    }
                    if (!matchedView) {
                        result.reset();  // 窗口不属于任何 shell 视图：未知
                    }
                    iter->Release();
                }
                enumerator->Release();
            }
        }
        windows->Release();
    }

    CoUninitialize();
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
