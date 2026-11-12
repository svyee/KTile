// KTile - Win32 平台层：自定义图标仓库
//
// 用户选中的图片（ico/png/jpg/bmp/gif）统一规范化：解码 → 按原图比例
// 缩放、最长边 128 → 编码 PNG 存入 <data>\res\。不裁剪、不改长宽比：
// 保住比例，渲染时铺满磁贴内框即可整图完整可见，一旦裁成正方形原始
// 构图永久丢失。最长边取 128 足够覆盖全部磁贴档位（渲染时降采样），
// PNG 保留 alpha，单文件几十 KB。item.icon 存相对 data 目录的路径
//（如 "res\a1b2c3d4.png"），整个目录拷走或换机图标依然有效。
//
#pragma once

#include "platform/win32/unique_handles.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 设置仓库根目录（= <exe>\data）。启动时由 app 层调用一次。
// 未设置时所有导入操作失败、相对路径原样返回（不影响读取绝对路径的老数据）。
void setIconStoreRoot(std::wstring dataDirectory);

// 当前仓库根目录（未设置时为空串）
std::wstring iconStoreRoot();

// 自定义图标目录（<root>\res）
std::wstring iconStoreDirectory();

// 把 item.icon 里存的值解析成磁盘绝对路径：
//   - 已经是绝对路径（含 UNC）→ 原样返回（兼容老数据 / 手动填的路径）
//   - 相对路径（"res\xx.png"）→ 相对 <data> 解析
std::wstring resolveStoredIconPath(const std::wstring& storedPath);

// 该存储值是否指向本程序托管的自定义图片。判据是相对路径（"res\xx.png"），
// 只有走「修改项目 → 选图」流水线存进来的才是它。老数据里手填的绝对路径
// 仍按系统图标处理，系统图标天生是方的，不应被当成用户图片铺满磁贴。
bool isManagedIconPath(const std::wstring& storedPath);

// 扩展名是否是可解码的图片（决定走 WIC 解码还是走 shell 图标提取）
bool isDecodableImageFile(const std::wstring& path);

// 解码后的像素缓冲。像素格式固定 32bpp BGRA、直通 alpha（非预乘）、
// 自上而下，0xAARRGGBB，与 WIC 的 GUID_WICPixelFormat32bppBGRA 内存布局
// 及 HICON 的 alpha 语义一致。AlphaBlend 需要预乘，由调用方建立 DIB 时
// 转换（见对话框预览）。
struct ImagePixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    bool valid() const noexcept {
        return width > 0 && height > 0 &&
               pixels.size() ==
                   static_cast<std::size_t>(width) *
                       static_cast<std::size_t>(height);
    }
};

// 解码图片文件（多帧文件取面积最大的一帧，例如多尺寸 ico）
bool decodeImageFile(const std::wstring& path, ImagePixels* out);

// 解码图片并按 cover 语义缩放到至少盖住 targetW×targetH，与 launcher
// 背景图的绘制方式配套：源图比需要的还大是纯浪费（3240×2025 壁纸解码
// 25MB，600×500 窗口最多用 1.2MB）。缩放在 WIC 解码管线里做（Fant 插值），
// 源图不足以盖住目标时保持原尺寸不放大，交给绘制端拉齐。
// 返回 false 的情形同 decodeImageFile（文件缺失/损坏/格式不支持）。
bool decodeImageFileCover(const std::wstring& path, std::uint32_t targetW,
                          std::uint32_t targetH, ImagePixels* out);

// 解码 HICON（用于预览「目标自身的图标」）
bool decodeIconPixels(HICON icon, ImagePixels* out);

// 把直通 alpha 的 BGRA 预乘成 D2D / AlphaBlend 需要的格式。半透明边缘
// 不预乘会泛白，这是 AlphaBlend(AC_SRC_ALPHA) 与 D2D 预乘位图的硬性要求。
// 像素序不变，仅 RGB 乘上 alpha/255。
std::vector<std::uint32_t> premultiplyBgra(const ImagePixels& pixels);

// 导入用户选择的图片。成功返回存储值（形如 "res\a1b2c3d4.png"），
// 失败返回空串并把原因写入 error（可为 nullptr）。
std::wstring importCustomIcon(const std::wstring& sourcePath,
                              std::wstring* error);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
