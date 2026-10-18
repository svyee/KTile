// 测试 Expected<T,E> 在 VS2017 / C++17 下的行为
#include "base/expected.h"

#include <cstdio>
#include <string>
#include <vector>
#include <utility>

using ktile::base::Expected;
using ktile::base::makeUnexpected;

enum class Err { Bad, Worse };

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            wprintf(L"FAIL %d: %s\n", __LINE__, L#cond);                 \
            ++g_failures;                                               \
        }                                                               \
    } while (false)

// 1) 基础：成功/失败
Expected<int, Err> parsePositive(int v) {
    if (v > 0) return v;
    return makeUnexpected(Err::Bad);
}

// 2) 非平凡类型
Expected<std::string, Err> makeString(bool ok) {
    if (ok) return std::string("hello");
    return makeUnexpected(Err::Worse);
}

// 3) void 特化
Expected<void, Err> doWork(bool ok) {
    if (ok) return {};
    return makeUnexpected(Err::Bad);
}

// 4) 移动语义
Expected<std::vector<int>, Err> makeVector() {
    std::vector<int> v{1, 2, 3};
    return v;  // 拷贝构造
}

int wmain() {
    // 基础成功
    auto a = parsePositive(5);
    CHECK(a.hasValue());
    CHECK(static_cast<bool>(a));
    CHECK(a.value() == 5);
    CHECK(*a == 5);

    // 基础失败
    auto b = parsePositive(-1);
    CHECK(!b.hasValue());
    CHECK(!static_cast<bool>(b));
    CHECK(b.error() == Err::Bad);

    // 非平凡类型
    auto c = makeString(true);
    CHECK(c && c.value() == "hello");
    auto d = makeString(false);
    CHECK(!d && d.error() == Err::Worse);

    // void 特化
    auto e = doWork(true);
    CHECK(e.hasValue());
    auto f = doWork(false);
    CHECK(!f && f.error() == Err::Bad);

    // 移动
    auto g = makeVector();
    CHECK(g && g->size() == 3);
    auto h = std::move(g);
    CHECK(h && h->size() == 3);

    // 拷贝赋值
    auto i = parsePositive(9);
    i = parsePositive(7);
    CHECK(i && i.value() == 7);
    i = parsePositive(0);
    CHECK(!i && i.error() == Err::Bad);

    // valueOr
    auto j = parsePositive(-5).valueOr(42);
    CHECK(j == 42);
    auto k = parsePositive(11).valueOr(42);
    CHECK(k == 11);

    // 错误带上下文（结构体错误类型）
    struct RichErr { int code; const char* msg; };
    Expected<int, RichErr> r = makeUnexpected(RichErr{42, "boom"});
    CHECK(!r && r.error().code == 42);

    if (g_failures == 0) {
        wprintf(L"===== EXPECTED: ALL PASSED =====\n");
    } else {
        wprintf(L"===== EXPECTED: %d FAILURES =====\n", g_failures);
    }
    return g_failures;
}
