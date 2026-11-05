// KTile - 基础设施层：运行日志
//
// 设计目标（对应「出问题把日志交给开发者分析」的诉求）：
//   - 每日滚动：logs\ktile-YYYYMMDD.log，只保留最近若干天，超限自动删除
//   - 体积小：单个文件设上限，写满当天停写并标注，磁盘占用有确定上界
//   - 钩子安全：info / warn 只写内存缓冲，绝不在调用点做文件 I/O ——
//     低级键盘钩子回调里一旦碰磁盘（杀毒软件扫描等），就可能超过
//     LowLevelHooksTimeout 被系统静默卸载（Win 键失灵的元凶之一）。
//     落盘统一由应用层的定时器调 flush() 完成；error 级别例外，
//     立即落盘（调用方必须保证不在钩子上下文里用 error）。
//   - 永不致命：任何内部错误都静默吞掉，日志再重要也不能让程序崩。
//
// 实现说明：
//   - 只用 C++17 标准库（fstream / filesystem），不依赖 Win32，
//     因此可以被单元测试覆盖，也符合 infrastructure 层的依赖约束。
//   - 全局单例式自由函数：埋点散落在 app / 平台层各处，
//     传 logger 指针会把所有调用链都拖上日志依赖，得不偿失。
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

// ---- 会话哨兵：识别「上次没有正常退出」（崩溃 / 被结束进程）----
//
// 原理：启动时创建 session.running，正常退出时删除。下次启动它还在，
// 就说明上一次没走到退出路径。只在正常交互模式下使用 —— 自动化
// （--debug-*）会并发起多个实例，哨兵会误报。
void markSessionStart();
void markSessionEnd();

}  // namespace applog
}  // namespace infrastructure
}  // namespace ktile
