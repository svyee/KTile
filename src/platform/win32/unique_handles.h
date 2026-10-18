// KTile - Win32 平台层：轻量 RAII 句柄封装
//
// 替代 WIL（Windows Implementation Library）。WIL 的部分头文件在 VS2017 下
// 有兼容性问题，且体积不小。这里只实现 KTile 实际需要的几种句柄。
//
#pragma once

#include <Windows.h>
#include <Unknwn.h>

#include <utility>

namespace ktile {
namespace platform {
namespace win32 {

// ---------------------------------------------------------------- 通用句柄
// 适用于 CloseHandle 类句柄（文件、事件、互斥体等）
class UniqueHandle {
public:
    UniqueHandle() noexcept : handle_(nullptr) {}
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~UniqueHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return valid(); }

    HANDLE release() noexcept {
        HANDLE temp = handle_;
        handle_ = nullptr;
        return temp;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE* put() noexcept {
        reset();
        return &handle_;
    }

private:
    HANDLE handle_;
};

// ---------------------------------------------------------------- 互斥体
// 用于单实例检测
class UniqueMutex {
public:
    UniqueMutex() noexcept : handle_(nullptr) {}

    UniqueMutex(const UniqueMutex&) = delete;
    UniqueMutex& operator=(const UniqueMutex&) = delete;

    UniqueMutex(UniqueMutex&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueMutex& operator=(UniqueMutex&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~UniqueMutex() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr; }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE* put() noexcept {
        reset();
        return &handle_;
    }

private:
    HANDLE handle_;
};

// ---------------------------------------------------------------- GDI 对象
//
// Deleter 必须返回 void —— 而 Win32 的 DestroyIcon / DeleteObject / DestroyMenu
// 都返回 BOOL。因此统一包一层 void 返回的适配器，绕开函数指针类型不匹配。
namespace detail {

inline void destroyIconAdapter(HICON value) noexcept {
    if (value != nullptr) {
        ::DestroyIcon(value);
    }
}

// HBITMAP 是 HGDIOBJ 的子类型，但函数指针不协变，必须各写一份。
inline void deleteBitmapAdapter(HBITMAP value) noexcept {
    if (value != nullptr) {
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(value));
    }
}

inline void deletePenAdapter(HPEN value) noexcept {
    if (value != nullptr) {
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(value));
    }
}

inline void deleteBrushAdapter(HBRUSH value) noexcept {
    if (value != nullptr) {
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(value));
    }
}

inline void deleteFontAdapter(HFONT value) noexcept {
    if (value != nullptr) {
        ::DeleteObject(reinterpret_cast<HGDIOBJ>(value));
    }
}

inline void destroyMenuAdapter(HMENU value) noexcept {
    if (value != nullptr) {
        ::DestroyMenu(value);
    }
}

}  // namespace detail

template <typename T, void(__stdcall* Deleter)(T)>
class UniqueGdiObject {
public:
    UniqueGdiObject() noexcept : object_(nullptr) {}
    explicit UniqueGdiObject(T object) noexcept : object_(object) {}

    UniqueGdiObject(const UniqueGdiObject&) = delete;
    UniqueGdiObject& operator=(const UniqueGdiObject&) = delete;

    UniqueGdiObject(UniqueGdiObject&& other) noexcept : object_(other.object_) {
        other.object_ = nullptr;
    }

    UniqueGdiObject& operator=(UniqueGdiObject&& other) noexcept {
        if (this != &other) {
            reset();
            object_ = other.object_;
            other.object_ = nullptr;
        }
        return *this;
    }

    ~UniqueGdiObject() { reset(); }

    T get() const noexcept { return object_; }
    bool valid() const noexcept { return object_ != nullptr; }

    T release() noexcept {
        T temp = object_;
        object_ = nullptr;
        return temp;
    }

    void reset(T object = nullptr) noexcept {
        if (object_ != nullptr) {
            Deleter(object_);
        }
        object_ = object;
    }

    T* put() noexcept {
        reset();
        return &object_;
    }

private:
    T object_;
};

// 具体别名
using UniqueHIcon = UniqueGdiObject<HICON, &detail::destroyIconAdapter>;
using UniqueHBITMAP = UniqueGdiObject<HBITMAP, &detail::deleteBitmapAdapter>;
using UniqueHPEN = UniqueGdiObject<HPEN, &detail::deletePenAdapter>;
using UniqueHBRUSH = UniqueGdiObject<HBRUSH, &detail::deleteBrushAdapter>;
using UniqueHFONT = UniqueGdiObject<HFONT, &detail::deleteFontAdapter>;
using UniqueHMenu = UniqueGdiObject<HMENU, &detail::destroyMenuAdapter>;

// ---------------------------------------------------------------- COM 指针
//
// 手写 COM 智能指针。不用 winrt::com_ptr（虽然它可用），
// 原因：keep 代码自洽、零额外依赖、更容易控制 Win7 行为。
//
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept : ptr_(nullptr) {}
    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {}

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_ != nullptr) {
            ptr_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            internalRelease();
            ptr_ = other.ptr_;
            if (ptr_ != nullptr) {
                ptr_->AddRef();
            }
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            internalRelease();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    ~ComPtr() { internalRelease(); }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    bool valid() const noexcept { return ptr_ != nullptr; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // 取地址用于接收输出参数（自动释放旧值）
    T** put() noexcept {
        internalRelease();
        return &ptr_;
    }

    void** putVoid() noexcept {
        internalRelease();
        return reinterpret_cast<void**>(&ptr_);
    }

    // 释放所有权
    T* release() noexcept {
        T* temp = ptr_;
        ptr_ = nullptr;
        return temp;
    }

    // 接管一个已 AddRef 的指针
    void attach(T* ptr) noexcept {
        internalRelease();
        ptr_ = ptr;
    }

    void reset() noexcept { internalRelease(); }

private:
    void internalRelease() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* ptr_;
};

// ---------------------------------------------------------------- 手写 COM 实现基类
//
// 替代 winrt::implements（后者在 VS2017 + 新 SDK 下无法编译）。
//
// 用法：
//   class MyDropTarget : public ComObjectBase<MyDropTarget, IDropTarget> {
//   public:
//       // 实现 IDropTarget 的四个方法（不要加 override，见下）
//       HRESULT __stdcall DragEnter(IDataObject*, DWORD, POINTL, DWORD*) noexcept
//       { return S_OK; }
//       ...
//   };
//
// 注意：派生类方法不要加 override 关键字 —— IDropTarget 是纯虚接口，
// 但 MSVC 19.16 对多重继承下的 override 检查较严格，省略更省事。
//
template <typename Derived>
class RefCounted {
public:
    ULONG __stdcall AddRef() noexcept {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG __stdcall Release() noexcept {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete static_cast<Derived*>(this);
            return 0;
        }
        return static_cast<ULONG>(count);
    }

protected:
    RefCounted() noexcept : refCount_(1) {}
    ~RefCounted() noexcept = default;

    LONG refCount_;
};

// 单接口 COM 对象基类
template <typename Derived, typename Interface>
class ComObjectBase : public Interface, public RefCounted<Derived> {
public:
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) noexcept {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *ppvObject = static_cast<Interface*>(static_cast<Derived*>(this));
            this->AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
