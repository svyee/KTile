// KTile - 基础设施层：运行日志
//
// 每日滚动（logs\ktile-YYYYMMDD.log，超期自动删除），单文件设上限，磁盘
// 占用有确定上界。钩子安全：info / warn 只写内存缓冲，绝不在调用点做
// 文件 I/O（低级键盘钩子回调碰磁盘可能超过 LowLevelHooksTimeout 被系统
// 静默卸载），落盘由应用层定时器调 flush()；error 立即落盘，调用方必须
// 保证不在钩子上下文使用。内部错误一律静默吞掉，日志不能让程序崩。
// 只用 C++17 标准库（不依赖 Win32），可单测；自由函数而非传 logger 指针。
//
#pragma once

#include <cstdint>
#include <string>

namespace ktile {
namespace infrastructure {
namespace applog {

enum class Level : std::uint8_t {
    Info = 0,
    Warn = 1,
    Error = 2,
};

// 初始化日志目录（不存在则创建），并清理超出保留期的旧日志。
// 重复调用会先 shutdown 再重来（测试用）。目录不可用时返回 false，
// 此后所有 write 都是安全的空操作。
bool initialize(const std::wstring& logDirectory);

// 冲刷并关闭当前文件。进程退出前调用。
void shutdown();

// 写一条日志（UTF-8）。线程安全。
// info / warn 纯内存缓冲；error 会立即落盘（勿在钩子回调中使用）。
void write(Level level, const std::string& messageUtf8);
void info(const std::string& messageUtf8);
void warn(const std::string& messageUtf8);
void error(const std::string& messageUtf8);

// 把缓冲落盘。应用层用低频定时器（秒级）驱动。
void flush();

bool isInitialized();

// 当前正在写入的文件路径（未初始化 / 未开文件时为空）。供测试与排查。
std::wstring currentFilePath();

// ---- 会话哨兵：识别上次未正常退出 ----
//
// 启动时创建 session.running，正常退出时删除；下次启动它还在就说明上次
// 没走到退出路径。只在正常交互模式下使用，自动化（--debug-*）会并发多个
// 实例，哨兵会误报。
void markSessionStart();
void markSessionEnd();

}  // namespace applog
}  // namespace infrastructure
}  // namespace ktile
