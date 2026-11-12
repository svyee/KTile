// KTile - Win32 平台层：OLE 拖放接收（把文件/文件夹/链接拖进面板）
//
// 用 OLE（RegisterDragDrop + IDropTarget）而不用 WM_DROPFILES：后者只在
// 松开时给一次消息，拖动中无法画落点反馈；需要 WS_EX_ACCEPTFILES 类样式，
// 与自绘无边框窗口冲突；且浏览器拖链接给的是 URL/纯文本，CF_HDROP 里
// 什么都没有。
// UIPI 限制：标准权限进程（资源管理器、浏览器）不能向管理员运行的窗口
// 投放，光标显示禁止符号，与是否注册 IDropTarget 无关。
// 代价：启动时需 OleInitialize（见 application.cpp），窗口销毁前 RevokeDragDrop。
// COM 类手写：项目在 VS2017 编译，winrt::implements 等模板基类在多重继承
// 下有覆盖风险（见 unique_handles.h 注解），IDropTarget 只有 4 个方法。
//
#pragma once

#include <Windows.h>
#include <objidl.h>
#include <oleidl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 拖进来的东西是什么
enum class DroppedKind : std::uint8_t {
    Path,  // 文件系统路径（资源管理器里拖来的）
    Url,   // 网址 / URL 协议串（浏览器里拖来的）
};

struct DroppedSource {
    DroppedKind kind = DroppedKind::Path;
    std::wstring value;
};

// 这份数据里有没有认识的东西（CF_HDROP / URL / 纯文本）。DragEnter 用它
// 决定显示可放置还是禁止光标，避免拖无关数据时窗口亮起却加不进去。
bool canAcceptDrop(IDataObject* dataObject) noexcept;

// 把拖拽数据里的内容抠出来（保持来源顺序）：先 CF_HDROP（可含多个），
// 再 UniformResourceLocatorW / UniformResourceLocator / CF_UNICODETEXT
//（浏览器地址栏、网页链接）。格式不认识时返回空 vector，调用方据此拒绝。
std::vector<DroppedSource> extractDroppedSources(IDataObject* dataObject);

// 拖拽数据里「可放置的条目数」（CF_HDROP 的文件个数）。
// 没有 CF_HDROP 时返回有没有 URL/文本（0 或 1）。用于在提示条上显示数量。
std::size_t countDroppedSources(IDataObject* dataObject) noexcept;

// 注册到窗口上的拖放接收器。
//
// 生命周期：窗口创建后 registerForWindow，窗口销毁前 revoke()。
class DropTarget {
public:
    // 松手时回调（sources 非空）。point 是屏幕坐标，供调用方判断落点。
    using DropHandler = std::function<void(std::vector<DroppedSource>, POINTL)>;

    // 悬停状态变化：entered=true 表示有东西拖到窗口上（应画落点预览），
    // false 表示离开/放下（撤掉预览）。count 是可放置条目数，point 是
    // 光标屏幕坐标，面板靠它算出新磁贴落点。
    using DragStateHandler =
        std::function<void(bool entered, std::size_t count, POINTL point)>;

    DropTarget() = default;
    ~DropTarget();

    DropTarget(const DropTarget&) = delete;
    DropTarget& operator=(const DropTarget&) = delete;

    // 注册拖放。失败（OLE 未初始化 / 已被别的组件占用）返回 false；不致命，
    // 只是拖放这一条入口不可用，其余功能照常。
    bool registerForWindow(HWND window,
                           DropHandler handler,
                           DragStateHandler stateHandler = nullptr);

    // 撤销注册并释放 COM 实现对象。可重复调用。
    void revoke() noexcept;

    bool registered() const noexcept { return window_ != nullptr; }

private:
    HWND window_ = nullptr;
    // 由 COM 引用计数管理：RevokeDragDrop 之后若调用方不再持有引用，
    // 它会自行 delete（见 Release 实现）。
    IDropTarget* implementation_ = nullptr;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
