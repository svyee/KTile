// KTile - 基础设施层：运行日志实现
#include "infrastructure/log/app_log.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace ktile {
namespace infrastructure {
namespace applog {
namespace {

// 日志文件保留天数。超过的 ktile-*.log 在启动 / 跨天时删除。
constexpr int kRetentionDays = 7;

// 单文件体积上限。写满后当天停写（留一行标注），磁盘占用上界为
// kRetentionDays × kMaxDailyBytes。
constexpr std::uintmax_t kMaxDailyBytes = 1024 * 1024;  // 1 MB

// 内存缓冲的丢弃上限：长时间没人 flush（异常路径）时也不能无限涨。
constexpr std::size_t kMaxBufferBytes = 64 * 1024;

constexpr const wchar_t* kSessionSentinelName = L"session.running";

// 所有字段都只在持有 mutex 时访问。
struct State {
    bool initialized = false;
    std::filesystem::path directory;
    std::ofstream stream;
    int openDate = 0;             // 当前打开文件对应的日期（YYYYMMDD）
    std::uintmax_t dayBytes = 0;  // 当天已写字节（含打开前已存在的部分）
    bool dayFull = false;         // 当天已触顶
    std::string buffer;           // 待落盘内容
};

std::mutex g_mutex;
State g_state;

// 本地时间的 YYYYMMDD（整数，便于比较与拼文件名）
int localDateInt(const std::tm& tm) {
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

std::tm localTimeOf(const std::chrono::system_clock::time_point& point) {
    const std::time_t time = std::chrono::system_clock::to_time_t(point);
    std::tm tm{};
    localtime_s(&tm, &time);
    return tm;
}

int todayInt() {
    return localDateInt(localTimeOf(std::chrono::system_clock::now()));
}

std::filesystem::path fileNameFor(const int dateInt) {
    wchar_t name[40];
    swprintf_s(name, L"ktile-%08d.log", dateInt);
    return std::filesystem::path(name);
}

// 行首：[HH:MM:SS.mmm] [级别]
std::string formatLine(const Level level, const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const std::tm tm = localTimeOf(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;

    const char* tag = "[I]";
    switch (level) {
        case Level::Info:  tag = "[I]"; break;
        case Level::Warn:  tag = "[W]"; break;
        case Level::Error: tag = "[E]"; break;
    }

    char head[48];
    snprintf(head, sizeof(head), "[%02d:%02d:%02d.%03d] %s ",
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             static_cast<int>(millis.count()), tag);
    return std::string(head) + message + "\n";
}

// 删除保留期之外的旧日志。失败静默（日志不能影响主功能）。
void cleanupOldLogsLocked(const int today) {
    std::error_code ec;
    if (!std::filesystem::exists(g_state.directory, ec)) {
        return;
    }
    // 「N 天前 0 点」的日期整数：借 mktime 做日历归一化
    std::tm tm = localTimeOf(std::chrono::system_clock::now());
    tm.tm_mday -= (kRetentionDays - 1);
    const std::time_t cutoffTime = std::mktime(&tm);
    std::tm cutoffTm{};
    localtime_s(&cutoffTm, &cutoffTime);
    const int cutoffInt = localDateInt(cutoffTm);

    for (const auto& entry :
         std::filesystem::directory_iterator(g_state.directory, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        int date = 0;
        if (swscanf_s(name.c_str(), L"ktile-%8d.log", &date) == 1 &&
            date >= 19000101 && date < cutoffInt) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

// 确保当天的文件已打开；跨天时滚动并清理旧文件。
bool ensureOpenLocked(const int today) {
    if (g_state.stream.is_open() && g_state.openDate == today) {
        return true;
    }
    if (g_state.stream.is_open()) {
        g_state.stream.close();  // 跨天：合上昨天的
    }
    g_state.dayFull = false;
    g_state.openDate = today;
    cleanupOldLogsLocked(today);

    const std::filesystem::path path = g_state.directory / fileNameFor(today);
    std::error_code ec;
    g_state.dayBytes = std::filesystem::exists(path, ec)
                           ? std::filesystem::file_size(path, ec)
                           : 0;
    if (ec) {
        g_state.dayBytes = 0;
    }
    g_state.stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
    return g_state.stream.is_open();
}

void flushLocked() {
    if (!g_state.initialized || g_state.buffer.empty()) {
        return;
    }
    if (!ensureOpenLocked(todayInt())) {
        return;  // 磁盘不可写：缓冲留着下次再试（受 kMaxBufferBytes 约束）
    }
    if (g_state.dayFull) {
        g_state.buffer.clear();
        return;
    }
    g_state.stream << g_state.buffer;
    g_state.stream.flush();
    g_state.dayBytes += g_state.buffer.size();
    g_state.buffer.clear();

    if (g_state.dayBytes >= kMaxDailyBytes) {
        g_state.stream << formatLine(
            Level::Warn, "当日日志已达体积上限，剩余部分不再记录");
        g_state.stream.flush();
        g_state.dayFull = true;
    }
}

std::filesystem::path sentinelPath() {
    return g_state.directory / kSessionSentinelName;
}

void writeLocked(const Level level, const std::string& message) {
    if (!g_state.initialized) {
        return;
    }
    if (g_state.buffer.size() >= kMaxBufferBytes) {
        // 缓冲满：直接丢。留一行说明比静默丢更容易排查。
        if (g_state.buffer.size() < kMaxBufferBytes + 1024) {
            g_state.buffer += formatLine(Level::Warn, "日志缓冲溢出，部分条目被丢弃");
        }
        return;
    }
    g_state.buffer += formatLine(level, message);
    // 只有 error 在调用点落盘（调用方保证不在钩子上下文用 error），
    // info / warn 一律等定时 flush，见头文件「钩子安全」说明。
    if (level == Level::Error) {
        flushLocked();
    }
}

}  // namespace

bool initialize(const std::wstring& logDirectory) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_state.stream.is_open()) {
        g_state.stream.close();
    }
    g_state.initialized = false;
    g_state.dayBytes = 0;
    g_state.dayFull = false;
    g_state.openDate = 0;
    g_state.buffer.clear();

    std::error_code ec;
    std::filesystem::create_directories(logDirectory, ec);
    if (!std::filesystem::is_directory(logDirectory, ec)) {
        return false;
    }
    g_state.directory = logDirectory;
    g_state.initialized = true;
    cleanupOldLogsLocked(todayInt());
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    flushLocked();
    if (g_state.stream.is_open()) {
        g_state.stream.close();
    }
    g_state.initialized = false;
}

void write(const Level level, const std::string& messageUtf8) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        writeLocked(level, messageUtf8);
    } catch (...) {
        // 日志绝不能让程序崩
    }
}

void info(const std::string& messageUtf8) {
    write(Level::Info, messageUtf8);
}

void warn(const std::string& messageUtf8) {
    write(Level::Warn, messageUtf8);
}

void error(const std::string& messageUtf8) {
    write(Level::Error, messageUtf8);
}

void flush() {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        flushLocked();
    } catch (...) {
    }
}

bool isInitialized() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.initialized;
}

std::wstring currentFilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) {
        return std::wstring();
    }
    return (g_state.directory / fileNameFor(todayInt())).wstring();
}

void markSessionStart() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(sentinelPath(), ec)) {
        writeLocked(Level::Warn,
                    "上次运行未正常退出（崩溃或被外部结束进程）");
    }
    // 哨兵内容无意义，存在即可
    std::ofstream sentinel(sentinelPath(), std::ios::out | std::ios::trunc);
    sentinel << "running\n";
}

void markSessionEnd() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(sentinelPath(), ec);
}

}  // namespace applog
}  // namespace infrastructure
}  // namespace ktile
