#include "platform/win32/drop_target.h"

#include <shellapi.h>

#include <cwchar>

#pragma comment(lib, "shell32.lib")

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// ---------------------------------------------------------------- 工具

// 把 HGLOBAL 包成 RAII 的锁定视图（配对调用 GlobalLock/GlobalUnlock）。
//
// 为什么不用 wil::unique_hglobal_locked：项目不引 WIL（体积与依赖约束），
// 手动包一层只有几行。
class ScopedGlobalLock {
public:
    explicit ScopedGlobalLock(const HGLOBAL handle) noexcept : handle_(handle) {
        if (handle_ != nullptr) {
            pointer_ = ::GlobalLock(handle_);
        }
    }

    ~ScopedGlobalLock() {
        if (pointer_ != nullptr) {
            ::GlobalUnlock(handle_);
        }
    }

    ScopedGlobalLock(const ScopedGlobalLock&) = delete;
    ScopedGlobalLock& operator=(const ScopedGlobalLock&) = delete;

    const void* get() const noexcept { return pointer_; }

private:
    HGLOBAL handle_ = nullptr;
    void* pointer_ = nullptr;
};

// STGMEDIUM 的 RAII 包装：GetData 成功拿到的数据必须用 ReleaseStgMedium 归还，
// 否则会泄漏（HGLOBAL 形式的 medium 尤其明显）。
class ScopedStgMedium {
public:
    ScopedStgMedium() { medium_.tymed = TYMED_NULL; }
    ~ScopedStgMedium() { ReleaseStgMedium(&medium_); }

    ScopedStgMedium(const ScopedStgMedium&) = delete;
    ScopedStgMedium& operator=(const ScopedStgMedium&) = delete;

    STGMEDIUM* put() noexcept { return &medium_; }
    const STGMEDIUM& get() const noexcept { return medium_; }

private:
    STGMEDIUM medium_{};
};

FORMATETC globalFormat(const CLIPFORMAT format) noexcept {
    FORMATETC descriptor{};
    descriptor.cfFormat = format;
    descriptor.ptd = nullptr;
    descriptor.dwAspect = DVASPECT_CONTENT;
    descriptor.lindex = -1;
    descriptor.tymed = TYMED_HGLOBAL;
    return descriptor;
}

bool supportsFormat(IDataObject* dataObject, const CLIPFORMAT format) noexcept {
    if (dataObject == nullptr || format == 0) {
        return false;
    }
    FORMATETC descriptor = globalFormat(format);
    return SUCCEEDED(dataObject->QueryGetData(&descriptor));
}

// 浏览器给的 URL 格式名（RFC 规定的两个剪贴板格式）
constexpr const wchar_t* kInternetUrlWide = L"UniformResourceLocatorW";
constexpr const wchar_t* kInternetUrlAnsi = L"UniformResourceLocator";

CLIPFORMAT internetUrlWideFormat() noexcept {
    return static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(kInternetUrlWide));
}

CLIPFORMAT internetUrlAnsiFormat() noexcept {
    return static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(kInternetUrlAnsi));
}

std::wstring trimWhitespace(std::wstring value) {
    const std::wstring whitespace = L" \t\r\n\v\f";
    const std::size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

// 读一个「宽字符文本」格式的 HGLOBAL。
std::wstring readWideText(IDataObject* dataObject, const CLIPFORMAT format) {
    if (dataObject == nullptr || format == 0) {
        return std::wstring();
    }

    FORMATETC descriptor = globalFormat(format);
    ScopedStgMedium medium;
    if (FAILED(dataObject->GetData(&descriptor, medium.put()))) {
        return std::wstring();
    }
    if (medium.get().tymed != TYMED_HGLOBAL ||
        medium.get().hGlobal == nullptr) {
        return std::wstring();
    }

    ScopedGlobalLock locked(medium.get().hGlobal);
    const auto* text = static_cast<const wchar_t*>(locked.get());
    if (text == nullptr) {
        return std::wstring();
    }
    const std::size_t capacity =
        GlobalSize(medium.get().hGlobal) / sizeof(wchar_t);
    const std::size_t length = wcsnlen_s(text, capacity);
    if (length == 0) {
        return std::wstring();
    }
    return trimWhitespace(std::wstring(text, length));
}

// 读 ANSI 文本格式（老程序/老浏览器只提供 ANSI 版 URL）。
std::wstring readAnsiText(IDataObject* dataObject, const CLIPFORMAT format) {
    if (dataObject == nullptr || format == 0) {
        return std::wstring();
    }

    FORMATETC descriptor = globalFormat(format);
    ScopedStgMedium medium;
    if (FAILED(dataObject->GetData(&descriptor, medium.put()))) {
        return std::wstring();
    }
    if (medium.get().tymed != TYMED_HGLOBAL ||
        medium.get().hGlobal == nullptr) {
        return std::wstring();
    }

    ScopedGlobalLock locked(medium.get().hGlobal);
    const auto* text = static_cast<const char*>(locked.get());
    if (text == nullptr) {
        return std::wstring();
    }
    const std::size_t capacity = GlobalSize(medium.get().hGlobal);
    const std::size_t length = strnlen_s(text, capacity);
    if (length == 0) {
        return std::wstring();
    }

    // 按系统 ANSI 代码页解码 —— 这类格式本身就是 ANSI 语义
    const int wideLength = MultiByteToWideChar(
        CP_ACP, 0, text, static_cast<int>(length), nullptr, 0);
    if (wideLength <= 0) {
        return std::wstring();
    }
    std::wstring value(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(length),
                            value.data(), wideLength) != wideLength) {
        return std::wstring();
    }
    return trimWhitespace(std::move(value));
}

// 取出 CF_HDROP 里的路径（保持来源顺序）。
std::vector<std::wstring> readDroppedPaths(IDataObject* dataObject) {
    std::vector<std::wstring> paths;
    if (dataObject == nullptr) {
        return paths;
    }

    FORMATETC descriptor = globalFormat(CF_HDROP);
    ScopedStgMedium medium;
    if (FAILED(dataObject->GetData(&descriptor, medium.put()))) {
        return paths;
    }
    if (medium.get().tymed != TYMED_HGLOBAL ||
        medium.get().hGlobal == nullptr) {
        return paths;
    }

    // CF_HDROP 的 HGLOBAL 直接就是一个 HDROP，
    // 用 DragQueryFileW 解析（不要自己解 DROPFILES 结构体 —— 长路径、
    // 多文件、Unicode/ANSI 两种布局都由它负责）。
    const auto drop = reinterpret_cast<HDROP>(medium.get().hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring path(static_cast<std::size_t>(length) + 1U, L'\0');
        const UINT copied =
            DragQueryFileW(drop, index, &path[0], length + 1U);
        if (copied == 0) {
            continue;
        }
        path.resize(copied);
        paths.push_back(std::move(path));
    }
    return paths;
}

// 从 URL / 文本格式里取一个值（按优先级尝试三种格式）。
std::wstring readDroppedUrl(IDataObject* dataObject) {
    if (dataObject == nullptr) {
        return std::wstring();
    }

    std::wstring value = readWideText(dataObject, internetUrlWideFormat());
    if (!value.empty()) {
        return value;
    }
    value = readWideText(dataObject, internetUrlAnsiFormat());
    if (!value.empty()) {
        return value;
    }
    // 纯文本兜底：有些来源只给 CF_UNICODETEXT。
    // 不校验它「像不像网址」—— 用户拖一段路径文本过来往往也是想要的，
    // 类型推断交给 ui::inferItemType。
    return readWideText(dataObject, CF_UNICODETEXT);
}

// ---------------------------------------------------------------- IDropTarget

class OleDropTarget final : public IDropTarget {
public:
    OleDropTarget(DropTarget::DropHandler handler,
                  DropTarget::DragStateHandler stateHandler)
        : handler_(std::move(handler)), stateHandler_(std::move(stateHandler)) {}

    // ---- IUnknown ----

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) noexcept {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
            *ppvObject = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() noexcept {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG __stdcall Release() noexcept {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(count);
    }

    // ---- IDropTarget ----

    HRESULT __stdcall DragEnter(IDataObject* dataObject, DWORD /*keyState*/,
                                POINTL point,
                                DWORD* effect) noexcept {
        canDrop_ = canAcceptDrop(dataObject);
        pendingCount_ = canDrop_ ? countDroppedSources(dataObject) : 0;
        lastPoint_ = point;
        setEffect(effect);
        notifyState(true);
        return S_OK;
    }

    HRESULT __stdcall DragOver(DWORD /*keyState*/, POINTL point,
                               DWORD* effect) noexcept {
        // 不改 canDrop_：进窗口时的判断已经定了，这里只回报效果。
        setEffect(effect);
        // 顺手把「还悬停在窗口上」再回报一次 —— UI 侧据此更新落点格
        // 并续上看门狗定时器（见 setFileDropTarget）：没有这一下，
        // 悬停期间落点框不跟手，看门狗还会把预览误清。
        lastPoint_ = point;
        notifyState(true);
        return S_OK;
    }

    HRESULT __stdcall DragLeave() noexcept {
        canDrop_ = false;
        pendingCount_ = 0;
        notifyState(false);
        return S_OK;
    }

    HRESULT __stdcall Drop(IDataObject* dataObject, DWORD /*keyState*/,
                           POINTL point, DWORD* effect) noexcept {
        try {
            std::vector<DroppedSource> sources =
                extractDroppedSources(dataObject);

            // 拖拽结束：无论成败都先撤掉高亮
            notifyState(false);
            canDrop_ = false;
            pendingCount_ = 0;

            if (sources.empty()) {
                setEffect(effect);
                return S_OK;
            }

            // 松手就表示「我就要放这里」：直接给 COPY 效果
            if (effect != nullptr) {
                *effect = DROPEFFECT_COPY;
            }
            if (handler_) {
                handler_(std::move(sources), point);
            }
            return S_OK;
        } catch (...) {
            // COM 边界不能抛异常出去 —— ole32 的调用方不会接。
            canDrop_ = false;
            pendingCount_ = 0;
            if (effect != nullptr) {
                *effect = DROPEFFECT_NONE;
            }
            return E_FAIL;
        }
    }

private:
    void setEffect(DWORD* effect) const noexcept {
        if (effect == nullptr) {
            return;
        }
        // 只接受「复制」。拖放语义是「往面板里加一份快捷方式」，
        // 绝不动源文件（*effect 里可能同时置了 MOVE 位）。
        *effect = (canDrop_ && ((*effect & DROPEFFECT_COPY) != 0))
                      ? DROPEFFECT_COPY
                      : DROPEFFECT_NONE;
    }

    void notifyState(const bool entered) noexcept {
        if (!stateHandler_) {
            return;
        }
        try {
            stateHandler_(entered, entered ? pendingCount_ : 0U, lastPoint_);
        } catch (...) {
            // 回调是 UI 层的事，出问题也不该让拖放链路崩掉
        }
    }

    LONG refCount_ = 1;
    bool canDrop_ = false;
    std::size_t pendingCount_ = 0;
    // 最近一次拿到的光标屏幕坐标（DragLeave 不带坐标，沿用上一次）
    POINTL lastPoint_{};
    DropTarget::DropHandler handler_;
    DropTarget::DragStateHandler stateHandler_;
};

}  // namespace

// ---------------------------------------------------------------- 对外接口

bool canAcceptDrop(IDataObject* dataObject) noexcept {
    if (dataObject == nullptr) {
        return false;
    }
    if (supportsFormat(dataObject, CF_HDROP)) {
        return true;
    }
    if (supportsFormat(dataObject, CF_UNICODETEXT)) {
        return true;
    }
    if (supportsFormat(dataObject, internetUrlWideFormat())) {
        return true;
    }
    return supportsFormat(dataObject, internetUrlAnsiFormat());
}

std::vector<DroppedSource> extractDroppedSources(IDataObject* dataObject) {
    std::vector<DroppedSource> sources;
    if (dataObject == nullptr) {
        return sources;
    }

    for (std::wstring& path : readDroppedPaths(dataObject)) {
        DroppedSource source;
        source.kind = DroppedKind::Path;
        source.value = std::move(path);
        sources.push_back(std::move(source));
    }
    if (!sources.empty()) {
        return sources;
    }

    std::wstring url = readDroppedUrl(dataObject);
    if (!url.empty()) {
        DroppedSource source;
        source.kind = DroppedKind::Url;
        source.value = std::move(url);
        sources.push_back(std::move(source));
    }
    return sources;
}

std::size_t countDroppedSources(IDataObject* dataObject) noexcept {
    if (dataObject == nullptr) {
        return 0;
    }
    if (supportsFormat(dataObject, CF_HDROP)) {
        FORMATETC descriptor = globalFormat(CF_HDROP);
        ScopedStgMedium medium;
        if (SUCCEEDED(dataObject->GetData(&descriptor, medium.put())) &&
            medium.get().tymed == TYMED_HGLOBAL &&
            medium.get().hGlobal != nullptr) {
            const auto drop = reinterpret_cast<HDROP>(medium.get().hGlobal);
            return static_cast<std::size_t>(
                DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0));
        }
        return 0;
    }
    return canAcceptDrop(dataObject) ? 1U : 0U;
}

DropTarget::~DropTarget() {
    revoke();
}

bool DropTarget::registerForWindow(const HWND window,
                                   DropHandler handler,
                                   DragStateHandler stateHandler) {
    revoke();
    if (window == nullptr || !handler) {
        return false;
    }

    // 引用计数从 1 开始（构造函数里），RegisterDragDrop 内部会再 AddRef；
    // 因此这里成功后必须把自己持有的那一份交出去一份引用给成员指针，
    // 失败则释放，避免泄漏。
    auto* target = new OleDropTarget(std::move(handler),
                                     std::move(stateHandler));

    const HRESULT result = RegisterDragDrop(window, target);
    if (FAILED(result)) {
        target->Release();
        return false;
    }

    window_ = window;
    implementation_ = target;
    return true;
}

void DropTarget::revoke() noexcept {
    if (window_ != nullptr) {
        RevokeDragDrop(window_);
        window_ = nullptr;
    }
    if (implementation_ != nullptr) {
        implementation_->Release();
        implementation_ = nullptr;
    }
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
