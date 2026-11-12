// KTile - Win32 平台层：单实例控制
//
// 通过命名互斥体确保同一时间只有一个实例运行。
// 第二个实例启动时会让已有实例把面板唤出来。
//
// Win7 兼容：CreateMutexW / SetForegroundWindow 均为老 API。
//
#pragma once

#include "platform/win32/win32_error.h"
#include "platform/win32/unique_handles.h"

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

class SingleInstance {
public:
    SingleInstance() = default;

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // 尝试成为唯一实例。
    // 返回 true  表示本进程是第一个实例（应继续启动）
    // 返回 false 表示已有实例在运行（应激活它并退出）
    bool tryAcquire(const std::wstring& mutexName);

    // 让已有实例把面板唤出来。
    //
    // 首选：给已有实例的消息窗口（按类名找）投唤出消息，通路与热键一致，
    // 面板照常 show() 并装好键盘钩子。直接 ShowWindow 主窗口是两回事，
    // 会留下没装钩子、内部状态仍认为自己是隐藏的窗口。
    //
    // 兜底：找不到消息窗口时，按标题找主窗口，ShowWindow 抢到前台。
    static void activateExistingInstance(const std::wstring& messageWindowClass,
                                         const std::wstring& windowTitle,
                                         const UINT activationMessage,
                                         const WPARAM activationParam);

    // 释放互斥体
    void release();

private:
    UniqueMutex mutex_;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
