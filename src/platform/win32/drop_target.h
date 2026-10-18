// KTile - Win32 平台层：OLE 拖放接收（把文件/文件夹/链接拖进面板）
//
// 为什么用 OLE 拖放（RegisterDragDrop + IDropTarget）而不是 WM_DROPFILES：
//   1. WM_DROPFILES 只在「松开鼠标」时才给一次消息：拖动过程中光标扫过
//      窗口时，用户看不到任何「这里能放、会放到哪一格」的反馈，
//      容易以为拖错地方了；IDropTarget 的 DragEnter/DragOver/DragLeave
//      能让我们实时算出落点格并画出来。
//   2. DragAcceptFiles 需要在窗口类样式上加 WS_EX_ACCEPTFILES，
//      而本窗口用的是自绘无边框类，改类样式会牵动焦点与外框行为。
//   3. 拖动来源不只有资源管理器：浏览器里拖链接/图片过来时给的是
//      URL 格式或纯文本，CF_HDROP 里什么都没有 —— 只认 WM_DROPFILES
//      会把这类来源整个漏掉。
//
// 注意：Windows 的 UIPI（用户界面特权隔离）不允许**标准权限**进程
// （资源管理器、浏览器）向**以管理员身份运行**的窗口投放拖放 ——
// 那种情况下光标会显示「禁止」符号，与目标窗口是否正确注册 IDropTarget
// 无关。要验证拖放，请以普通权限启动本程序。
//
// 代价：必须在启动时 OleInitialize（见 application.cpp），
// 且窗口销毁前要 RevokeDragDrop。
//
// 本文件里的 COM 类是**手写**的：项目在 VS2017 上编译，
// winrt::implements / ComObjectBase 这类模板基类在多重继承下
// 有「纯虚函数没被认定被覆盖」的风险（详见 unique_handles.h 的注解），
// IDropTarget 只有 4 个方法，手写一遍最省事也最稳。
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

// 这份拖拽数据里有没有我们认识的东西（CF_HDROP / URL / 纯文本）。
//
// DragEnter 用它决定显示「可放置」还是「禁止」光标，
// 也避免了「拖一段无关数据过来，窗口亮起来却什么都加不进去」的假承诺。
bool canAcceptDrop(IDataObject* dataObject) noexcept;

// 把拖拽数据里的内容抠出来（保持来源顺序）。
//
// 解析顺序：
//   1. CF_HDROP        —— 资源管理器拖文件/文件夹都是它，可含多个
//   2. UniformResourceLocatorW / UniformResourceLocator / CF_UNICODETEXT
//                      —— 浏览器地址栏、网页里的链接
//
// 数据格式不认识时返回空 vector（调用方据此拒绝这次拖放）。
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

    // 拖拽悬停状态变化：entered=true 表示有东西拖到了窗口上（应画落点预览），
    // false 表示离开了/放下了（撤掉预览）。count 是可放置的条目数。
    //
    // point 是当前光标的**屏幕坐标** —— 面板靠它算出「新磁贴会落在哪一格」，
    // 这样拖进来时就能直接看到落点，而不是只有一句「松手即可添加」。
    using DragStateHandler =
        std::function<void(bool entered, std::size_t count, POINTL point)>;

    DropTarget() = default;
    ~DropTarget();

    DropTarget(const DropTarget&) = delete;
    DropTarget& operator=(const DropTarget&) = delete;

    // 注册拖放。失败（OLE 未初始化 / 已被别的组件占用）返回 false —— 不致命，
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
