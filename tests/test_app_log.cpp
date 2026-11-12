// KTile - 运行日志测试
//
// 覆盖：初始化建目录与写入 flush、保留期清理、单日体积上限、
// 会话哨兵（异常退出检测 / 正常退出清除）、未初始化写入安全空操作。
//
#include "infrastructure/log/app_log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ktile::infrastructure;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::filesystem::path freshDir(const wchar_t* name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

// 目录下唯一的 ktile-*.log（测试目录是新建的，最多一个）
std::filesystem::path onlyLogFile(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.find(L"ktile-") == 0 && name.find(L".log") != std::wstring::npos) {
            return entry.path();
        }
    }
    return {};
}

// --- 基本写入

void testWriteAndFlush() {
    const auto dir = freshDir(L"ktile_applog_test_basic");
    CHECK(applog::initialize(dir.wstring()));

    applog::info("第一条日志");
    applog::warn("警告内容 utf8 中文");
    // flush 后必须有内容
    applog::flush();

    const auto file = onlyLogFile(dir);
    CHECK(!file.empty());
    CHECK(file == std::filesystem::path(applog::currentFilePath()));

    const std::string content = readFile(file);
    CHECK(content.find("第一条日志") != std::string::npos);
    CHECK(content.find("[W] 警告内容") != std::string::npos);
    // 行首时间戳格式 [HH:MM:SS.mmm]
    CHECK(content.size() > 20 && content[0] == '[' && content[3] == ':');

    applog::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testErrorFlushesImmediately() {
    const auto dir = freshDir(L"ktile_applog_test_error");
    CHECK(applog::initialize(dir.wstring()));

    applog::error("严重错误立即落盘");
    // 不调 flush：error 必须在调用点落盘
    const auto file = onlyLogFile(dir);
    CHECK(!file.empty());
    CHECK(readFile(file).find("[E] 严重错误") != std::string::npos);

    applog::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// --- 保留期清理

void testRetentionCleanup() {
    const auto dir = freshDir(L"ktile_applog_test_retention");
    // 预置：远古日志、近期日志、无关文件、非日期名日志各一份
    { std::ofstream f(dir / L"ktile-20200101.log"); f << "old\n"; }
    { std::ofstream f(dir / L"ktile-20991231.log"); f << "future\n"; }
    { std::ofstream f(dir / L"notes.txt"); f << "keep\n"; }
    { std::ofstream f(dir / L"ktile-notadate.log"); f << "keep\n"; }

    CHECK(applog::initialize(dir.wstring()));

    std::error_code ec;
    CHECK(!std::filesystem::exists(dir / L"ktile-20200101.log", ec));
    CHECK(std::filesystem::exists(dir / L"ktile-20991231.log", ec));
    CHECK(std::filesystem::exists(dir / L"notes.txt", ec));
    CHECK(std::filesystem::exists(dir / L"ktile-notadate.log", ec));

    applog::shutdown();
    std::filesystem::remove_all(dir, ec);
}

// --- 体积上限

void testDailySizeCap() {
    const auto dir = freshDir(L"ktile_applog_test_cap");
    CHECK(applog::initialize(dir.wstring()));

    // 每行约 100 字节，12000 行 ≈ 1.2 MB 越过 1 MB 上限；分段 flush，
    // 否则一次性灌满只测到内存缓冲的 64KB 丢弃上限。
    const std::string payload(80, 'x');
    for (int i = 0; i < 12000; ++i) {
        applog::info(payload);
        if (i % 500 == 499) {
            applog::flush();
        }
    }
    applog::flush();

    const auto file = onlyLogFile(dir);
    CHECK(!file.empty());
    std::error_code ec;
    const auto sizeAfterCap = std::filesystem::file_size(file, ec);
    CHECK(!ec);
    CHECK(readFile(file).find("已达体积上限") != std::string::npos);

    // 触顶后再写：文件不再增长
    for (int i = 0; i < 100; ++i) {
        applog::info(payload);
    }
    applog::flush();
    CHECK(std::filesystem::file_size(file, ec) == sizeAfterCap);

    applog::shutdown();
    std::filesystem::remove_all(dir, ec);
}

// --- 会话哨兵

void testSessionSentinel() {
    const auto dir = freshDir(L"ktile_applog_test_sentinel");
    CHECK(applog::initialize(dir.wstring()));

    applog::markSessionStart();
    std::error_code ec;
    CHECK(std::filesystem::exists(dir / L"session.running", ec));

    // 哨兵还在时再 start：上次异常退出，必须留下警告
    applog::markSessionStart();
    applog::flush();
    const auto file = onlyLogFile(dir);
    CHECK(!file.empty());
    CHECK(readFile(file).find("未正常退出") != std::string::npos);

    applog::markSessionEnd();
    CHECK(!std::filesystem::exists(dir / L"session.running", ec));

    applog::shutdown();
    std::filesystem::remove_all(dir, ec);
}

// --- 未初始化

void testUninitializedIsSafe() {
    // 上个用例已 shutdown：写入必须无害
    applog::info("不应落盘");
    applog::error("也不应落盘");
    applog::flush();
    CHECK(!applog::isInitialized());
    CHECK(applog::currentFilePath().empty());
}

}  // namespace

int wmain() {
    std::printf("== app_log 测试 ==\n");
    testWriteAndFlush();
    testErrorFlushesImmediately();
    testRetentionCleanup();
    testDailySizeCap();
    testSessionSentinel();
    testUninitializedIsSafe();
    std::printf("完成：%d 项检查，%d 个失败\n", g_checks, g_failures);
    return g_failures;
}
