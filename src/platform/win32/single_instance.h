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
    // 首选：给已有实例的**消息窗口**（按类名找）投一条唤出消息。这条通路
    // 与热键完全一致 —— 面板会照常走 show()，一并装好键盘钩子、处理前台
    // 窗口关系；把它当成「按标题找主窗口再 ShowWindow」是两回事，后者会
    // 留下一个没装钩子、内部状态也认为自己是隐藏的窗口。
    //
    // 兜底：找不到消息窗口时，按窗口标题找主窗口，ShowWindow 抢到前台。
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
