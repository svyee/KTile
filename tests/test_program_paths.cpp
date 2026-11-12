// KTile - 以 exe 目录为基准的相对路径测试
//
// 判错的代价：裸命令被当成相对路径会变成「<exe目录>\notepad.exe 不存在」；
// 网址被当成相对路径就再也打不开；"..\bin\x.exe" 原样交给 ShellExecute
// 相对的是当前工作目录，而它由启动方式决定，绿色软件不能依赖。
//
#include "platform/win32/program_paths.h"

#include <cstdio>
#include <string>

namespace {

using namespace ktile::platform::win32;

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void testRelativeDetection() {
    // 带目录分隔符的相对写法：认
    CHECK(isProgramRelativePath(L"tools\\app.exe"));
    CHECK(isProgramRelativePath(L".\\tools\\app.exe"));
    CHECK(isProgramRelativePath(L"..\\bin\\app.exe"));
    CHECK(isProgramRelativePath(L"tools/app.exe"));   // 正斜杠也接受
    CHECK(isProgramRelativePath(L"  tools\\app.exe  "));

    // 绝对路径 / UNC / 盘符
    CHECK(!isProgramRelativePath(L"C:\\tools\\app.exe"));
    CHECK(!isProgramRelativePath(L"C:tools\\app.exe"));
    CHECK(!isProgramRelativePath(L"\\\\server\\share\\app.exe"));
    CHECK(!isProgramRelativePath(L"\\rooted\\app.exe"));

    // 裸名字交给系统（PATH / App Paths），URL 交给浏览器
    CHECK(!isProgramRelativePath(L"notepad.exe"));
    CHECK(!isProgramRelativePath(L"office.farxin.cn"));
    CHECK(!isProgramRelativePath(L"https://example.com/a/b"));
    CHECK(!isProgramRelativePath(L""));
}

void testResolve() {
    const std::wstring base = executableDirectory();
    CHECK(!base.empty());
    // 目录不带结尾反斜杠（否则拼出来的路径会有 "\\\\"）
    if (!base.empty()) {
        CHECK(base.back() != L'\\' && base.back() != L'/');
    }

    // 不相干的写法一律原样返回
    CHECK(resolveProgramRelativePath(L"C:\\tools\\app.exe") ==
          L"C:\\tools\\app.exe");
    CHECK(resolveProgramRelativePath(L"notepad.exe") == L"notepad.exe");
    CHECK(resolveProgramRelativePath(L"https://example.com/x") ==
          L"https://example.com/x");
    CHECK(resolveProgramRelativePath(L"") .empty());

    // 相对写法拼到 exe 目录下
    const std::wstring relative =
        resolveProgramRelativePath(L"tools\\app.exe");
    if (!base.empty()) {
        CHECK(relative == base + L"\\tools\\app.exe");
    }

    // ".." 必须被规范化掉（不能把 ".." 原样交给 ShellExecute，
    // 也不能让日志里出现 "<exe目录>\..\bin"）
    const std::wstring parent = resolveProgramRelativePath(L"..\\bin\\app.exe");
    CHECK(parent.find(L"..") == std::wstring::npos);
    if (!base.empty()) {
        // 规范化之后必定落在 exe 目录的上一级
        const std::size_t slash = base.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            CHECK(parent == base.substr(0, slash) + L"\\bin\\app.exe");
        }
    }
}

}  // namespace

int main() {
    testRelativeDetection();
    testResolve();

    std::printf("检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("===== 路径解析: 全部通过 =====\n");
        return 0;
    }
    std::printf("===== 路径解析: %d 项失败 =====\n", g_failures);
    return 1;
}
