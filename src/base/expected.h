// KTile - C++17 兼容层：Expected<T, E>
//
// 替代 C++23 的 std::expected。VS2017 (MSVC 19.16) 不支持 std::expected。
//
// 设计要点：
//  - 语义对齐 std::expected 的常用子集，未来若升级到 C++23 可平滑替换
//  - 支持 void 特化（Expected<void, E>）
//  - 不使用任何 C++17 之后的特性
//  - 不抛异常（noexcept 移动/析构），适配 /EHsc 下的确定性行为
//
// 用法：
//    Expected<int, MyError> f();
//    auto r = f();
//    if (r) { use(*r); } else { use(r.error()); }
//
#pragma once

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>

namespace ktile {
namespace base {

// ---------------------------------------------------------------- 错误包装
// 与 std::unexpected 对应，用于构造错误态。
template <typename E>
class Unexpected {
public:
    explicit Unexpected(const E& value) : value_(value) {}
    explicit Unexpected(E&& value) : value_(std::move(value)) {}

    const E& value() const& noexcept { return value_; }
    E& value() & noexcept { return value_; }
    E&& value() && noexcept { return std::move(value_); }

private:
    E value_;
};

// 推导辅助
template <typename E>
Unexpected<typename std::decay<E>::type> makeUnexpected(E&& error) {
    return Unexpected<typename std::decay<E>::type>(std::forward<E>(error));
}

// ---------------------------------------------------------------- Expected
template <typename T, typename E>
class Expected {
public:
    using ValueType = T;
    using ErrorType = E;

    // ---- 构造：成功态 ----
    Expected(const T& value) : hasValue_(true) { new (&storage_.value) T(value); }
    Expected(T&& value) : hasValue_(true) { new (&storage_.value) T(std::move(value)); }

    // ---- 构造：错误态 ----
    Expected(const Unexpected<E>& error) : hasValue_(false) {
        new (&storage_.error) E(error.value());
    }
    Expected(Unexpected<E>&& error) : hasValue_(false) {
        new (&storage_.error) E(std::move(error.value()));
    }

    // ---- 拷贝/移动 ----
    Expected(const Expected& other) : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&storage_.value) T(other.storage_.value);
        } else {
            new (&storage_.error) E(other.storage_.error);
        }
    }

    Expected(Expected&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value &&
        std::is_nothrow_move_constructible<E>::value)
        : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&storage_.value) T(std::move(other.storage_.value));
        } else {
            new (&storage_.error) E(std::move(other.storage_.error));
        }
    }

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&storage_.value) T(other.storage_.value);
            } else {
                new (&storage_.error) E(other.storage_.error);
            }
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value &&
        std::is_nothrow_move_constructible<E>::value) {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&storage_.value) T(std::move(other.storage_.value));
            } else {
                new (&storage_.error) E(std::move(other.storage_.error));
            }
        }
        return *this;
    }

    ~Expected() { destroy(); }

    // ---- 状态查询 ----
    bool hasValue() const noexcept { return hasValue_; }
    explicit operator bool() const noexcept { return hasValue_; }

    // ---- 取值 ----
    const T& value() const& noexcept {
        assert(hasValue_ && "Expected::value() called on error state");
        return storage_.value;
    }
    T& value() & noexcept {
        assert(hasValue_ && "Expected::value() called on error state");
        return storage_.value;
    }
    T&& value() && noexcept {
        assert(hasValue_ && "Expected::value() called on error state");
        return std::move(storage_.value);
    }

    const T& operator*() const& noexcept { return value(); }
    T& operator*() & noexcept { return value(); }
    T&& operator*() && noexcept { return std::move(value()); }

    const T* operator->() const noexcept { return &storage_.value; }
    T* operator->() noexcept { return &storage_.value; }

    // ---- 取错误 ----
    const E& error() const& noexcept {
        assert(!hasValue_ && "Expected::error() called on value state");
        return storage_.error;
    }
    E& error() & noexcept {
        assert(!hasValue_ && "Expected::error() called on value state");
        return storage_.error;
    }
    E&& error() && noexcept {
        assert(!hasValue_ && "Expected::error() called on value state");
        return std::move(storage_.error);
    }

    // ---- 带默认值取值 ----
    T valueOr(T fallback) const {
        return hasValue_ ? storage_.value : std::move(fallback);
    }

private:
    void destroy() noexcept {
        if (hasValue_) {
            storage_.value.~T();
        } else {
            storage_.error.~E();
        }
    }

    union Storage {
        Storage() {}
        ~Storage() {}
        T value;
        E error;
    };

    Storage storage_;
    bool hasValue_;
};

// ---------------------------------------------------------------- void 特化
template <typename E>
class Expected<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    Expected() noexcept : hasValue_(true) {}

    Expected(const Unexpected<E>& error) : hasValue_(false) {
        new (&storage_.error) E(error.value());
    }
    Expected(Unexpected<E>&& error) : hasValue_(false) {
        new (&storage_.error) E(std::move(error.value()));
    }

    Expected(const Expected& other) : hasValue_(other.hasValue_) {
        if (!hasValue_) {
            new (&storage_.error) E(other.storage_.error);
        }
    }

    Expected(Expected&& other) noexcept(std::is_nothrow_move_constructible<E>::value)
        : hasValue_(other.hasValue_) {
        if (!hasValue_) {
            new (&storage_.error) E(std::move(other.storage_.error));
        }
    }

    Expected& operator=(const Expected& other) {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (!hasValue_) {
                new (&storage_.error) E(other.storage_.error);
            }
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_constructible<E>::value) {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (!hasValue_) {
                new (&storage_.error) E(std::move(other.storage_.error));
            }
        }
        return *this;
    }

    ~Expected() { destroy(); }

    bool hasValue() const noexcept { return hasValue_; }
    explicit operator bool() const noexcept { return hasValue_; }

    void value() const noexcept {
        assert(hasValue_ && "Expected<void>::value() called on error state");
    }

    const E& error() const& noexcept {
        assert(!hasValue_ && "Expected<void>::error() called on value state");
        return storage_.error;
    }
    E& error() & noexcept {
        assert(!hasValue_ && "Expected<void>::error() called on value state");
        return storage_.error;
    }

private:
    void destroy() noexcept {
        if (!hasValue_) {
            storage_.error.~E();
        }
    }

    union Storage {
        Storage() {}
        ~Storage() {}
        E error;
    };

    Storage storage_;
    bool hasValue_;
};

// 便捷别名：无错误信息的成功/失败
inline constexpr bool kOk = true;

}  // namespace base
}  // namespace ktile
