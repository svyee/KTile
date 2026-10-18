// KTile - Win32 平台层：图标提取实现
#include "platform/win32/icon_loader.h"

#include "base/text_convert.h"
#include "platform/win32/icon_store.h"

#include <ShlObj.h>
#include <commoncontrols.h>  // IImageList / SHGetImageList
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 按档位取像素尺寸
int pixelSizeForHint(const IconSizeHint hint) {
    switch (hint) {
        case IconSizeHint::Small:  return 32;
        case IconSizeHint::Medium: return 64;
        case IconSizeHint::Large:  return 256;
    }
    return 64;
}

// 判断扩展名是否为 PE 模块（exe/dll/ico 可直接 ExtractIconEx）
bool hasIconBearingExtension(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return false;
    }
    std::wstring extension = path.substr(dot);
    for (auto& ch : extension) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return extension == L".exe" || extension == L".dll" ||
           extension == L".ico" || extension == L".cpl" ||
           extension == L".scr" || extension == L".ocx";
}

// 用 SHGetFileInfo 取图标（适合文档、文件夹、URL）。
// 返回的 HICON 由调用方负责销毁。
HICON shellIconForPath(const std::wstring& path, const bool isFolder) {
    SHFILEINFOW info{};
    const UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES;
    const DWORD attributes = isFolder ? FILE_ATTRIBUTE_DIRECTORY
                                      : FILE_ATTRIBUTE_NORMAL;

    // 直接请求大图标（32px）
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info),
                       flags | SHGFI_LARGEICON) &&
        info.hIcon != nullptr) {
        return info.hIcon;
    }

    // 退到小图标
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info),
                       flags | SHGFI_SMALLICON) &&
        info.hIcon != nullptr) {
        return info.hIcon;
    }

    return nullptr;
}

// 从 ImageList（系统图标列表）按索引取指定尺寸的图标。
// 这是拿到 256px jumbo 图标的唯一可靠方式。
HICON iconFromImageListId(const std::wstring& path,
                          const bool isFolder,
                          const int imageListId) {
    SHFILEINFOW info{};
    const DWORD attributes =
        isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    const UINT flags = SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES;

    IImageList* imageList = nullptr;
    if (FAILED(SHGetImageList(imageListId, IID_IImageList,
                              reinterpret_cast<void**>(&imageList))) ||
        imageList == nullptr) {
        return nullptr;
    }
    HICON icon = nullptr;
    if (SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info), flags)) {
        imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    }

    imageList->Release();
    return icon;
}

// 测量 HICON 非透明内容占画布的边长比（内容包围盒边长 / 画布边长）。
// 测量失败（取不到像素）返回 1.0 —— 宁可放过，不可误杀。
//
// 存在的意义：系统 jumbo ImageList 有一种退化情形——exe 的图标资源里
// 没有 256px 帧时，GetIcon 返回的 HICON 是「256x256 画布 + 48x48 小图
// 贴在左上角」（实测 i_view64.exe / Bandicam 均如此，内容占比仅 3.5%）。
// 这种图标铺到磁贴上就是一小点。此函数用于识别这种退化。
double iconContentRatio(const HICON icon) {
    if (icon == nullptr) {
        return 1.0;
    }

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        return 1.0;
    }

    BITMAP bitmap{};
    double ratio = 1.0;
    if (info.hbmColor != nullptr &&
        GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0 &&
        bitmap.bmWidth > 0 && bitmap.bmHeight > 0) {
        const int width = bitmap.bmWidth;
        const int height = bitmap.bmHeight;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        HDC screen = GetDC(nullptr);
        const int scanned = GetDIBits(screen, info.hbmColor, 0,
                                      static_cast<UINT>(height),
                                      pixels.data(), &bmi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);

        if (scanned != 0) {
            int minX = width, minY = height;
            int maxX = -1, maxY = -1;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const std::uint32_t pixel =
                        pixels[static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x)];
                    if ((pixel >> 24) >= 16) {  // 非透明像素
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (x > maxX) maxX = x;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            if (maxX >= 0 && width > 0 && height > 0) {
                const double contentSide = static_cast<double>(
                    (std::max)(maxX - minX + 1, maxY - minY + 1));
                const double canvasSide = static_cast<double>(
                    (std::max)(width, height));
                ratio = contentSide / canvasSide;
            }
        }
    }

    if (info.hbmColor != nullptr) {
        DeleteObject(info.hbmColor);
    }
    if (info.hbmMask != nullptr) {
        DeleteObject(info.hbmMask);
    }
    return ratio;
}

HICON iconFromSystemImageList(const std::wstring& path,
                              const bool isFolder,
                              const int desiredSize) {
    // SHIL_JUMBO/EXTRALARGE/LARGE 分别对应 256/48/32 px（Vista+）
    int imageListId = SHIL_LARGE;
    if (desiredSize >= 256) {
        imageListId = SHIL_JUMBO;
    } else if (desiredSize >= 48) {
        imageListId = SHIL_EXTRALARGE;
    }

    HICON icon = iconFromImageListId(path, isFolder, imageListId);

    // jumbo 退化兜底：256 画布里只装了 48 小图（见 iconContentRatio 注释）。
    // 退回 48px 的 EXTRALARGE —— 内容铺满画布，放进磁贴顶多放大发虚，
    // 远好过缩成一个小点。
    if (icon != nullptr && imageListId == SHIL_JUMBO &&
        iconContentRatio(icon) < 0.5) {
        DestroyIcon(icon);
        icon = iconFromImageListId(path, isFolder, SHIL_EXTRALARGE);
    }
    return icon;
}

// 直接从 PE 资源（exe/dll/…）按请求尺寸提取图标。
//
// 为什么优先于 ImageList：PrivateExtractIconsW 与资源管理器同源，
// 会挑图标资源里最接近请求尺寸的帧再缩放到请求尺寸 —— 实测对
// i_view64.exe / Bandicam 都能拿满 256x256（ImageList 的 jumbo 给的
// 却是 256 画布装 48 小图），对 notepad/chrome 等则与 jumbo 完全一致。
// 该 API 自 Windows 2000 起可用，Win7 无兼容问题。
HICON extractScaledPeIcon(const std::wstring& path, const int desiredSize) {
    HICON icon = nullptr;
    const UINT extracted = PrivateExtractIconsW(
        path.c_str(), 0, desiredSize, desiredSize, &icon, nullptr, 1, 0);
    if (extracted > 0 && icon != nullptr) {
        return icon;
    }
    return nullptr;
}

// 判断系统 ImageList 里的图标是否是「通用空白页」——
// 文件不存在/无关联时，Shell 会返回白页图标，这种图标没有辨识度，
// 不如退回按名称首字渲染。做法：统计非透明像素中「接近白/灰」的占比，
// 若几乎全是灰度色调（无彩色）则视为泛型图标。
bool looksGeneric(const std::wstring& path,
                  const bool isFolder,
                  const int desiredSize) {
    HICON icon = iconFromSystemImageList(path, isFolder, desiredSize);
    if (icon == nullptr) {
        return true;  // 取不到也算「没拿到有效图标」
    }

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        DestroyIcon(icon);
        return true;
    }

    BITMAP bitmap{};
    const bool hasColorBitmap =
        info.hbmColor != nullptr && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0;

    bool generic = true;
    if (hasColorBitmap && bitmap.bmWidth > 0 && bitmap.bmHeight > 0) {
        const int width = bitmap.bmWidth;
        const int height = bitmap.bmHeight;

        // 取 32bpp DIB 像素
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // 自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

        HDC screen = GetDC(nullptr);
        const int scanned = GetDIBits(screen, info.hbmColor, 0,
                                      static_cast<UINT>(height),
                                      pixels.data(), &bmi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);

        if (scanned != 0) {
            int opaque = 0;
            int chromatic = 0;
            for (const std::uint32_t pixel : pixels) {
                const int b = static_cast<int>(pixel & 0xFFU);
                const int g = static_cast<int>((pixel >> 8) & 0xFFU);
                const int r = static_cast<int>((pixel >> 16) & 0xFFU);
                const int a = static_cast<int>((pixel >> 24) & 0xFFU);
                if (a < 40) {
                    continue;  // 透明区域不计
                }
                ++opaque;
                const int maxChannel = (std::max)(r, (std::max)(g, b));
                const int minChannel = (std::min)(r, (std::min)(g, b));
                if (maxChannel - minChannel > 24) {
                    ++chromatic;
                }
            }
            // 有相当比例的不透明像素、且其中绝大多数无彩色 → 泛型白页图标
            if (opaque > 64) {
                const double chromaticRatio =
                    static_cast<double>(chromatic) / static_cast<double>(opaque);
                generic = chromaticRatio < 0.06;
            }
        }
    }

    if (info.hbmColor != nullptr) {
        DeleteObject(info.hbmColor);
    }
    if (info.hbmMask != nullptr) {
        DeleteObject(info.hbmMask);
    }
    DestroyIcon(icon);
    return generic;
}

// 把解码出的像素（32bpp BGRA，直通 alpha）做成 HICON。
//
// 走 DIB + CreateIconIndirect 而不是 CreateIconFromResource：
// 前者是内存里的位图，与「文件里读出来的像素」天然对口。
// 掩码位图必须给（哪怕是全 0）—— 32bpp 图标由 alpha 通道决定透明度，
// 掩码只是历史遗留字段，但 CreateIconIndirect 要求它非空。
HICON iconFromPixels(const ImagePixels& image) {
    if (!image.valid()) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;  // 负数 = 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color =
        CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) {
            DeleteObject(color);
        }
        return nullptr;
    }
    std::memcpy(bits, image.pixels.data(),
                image.pixels.size() * sizeof(std::uint32_t));

    // 1bpp 掩码：每行按 4 字节对齐（((w+15)/16)*2 字节），全 0
    const std::size_t maskStride =
        ((static_cast<std::size_t>(image.width) + 15U) / 16U) * 2U;
    std::vector<std::uint8_t> maskBits(maskStride *
                                       static_cast<std::size_t>(image.height),
                                       0U);
    HBITMAP mask = CreateBitmap(image.width, image.height, 1, 1,
                                maskBits.data());

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;

    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(color);
    if (mask != nullptr) {
        DeleteObject(mask);
    }
    return icon;
}

}  // namespace

UniqueHIcon loadIconFromFile(const std::wstring& filePath,
                             const IconSizeHint hint) {
    if (filePath.empty()) {
        return UniqueHIcon();
    }

    const int desired = pixelSizeForHint(hint);

    // ---- 方式零：位图文件（png / jpg / jpeg / bmp / gif / tif / ico）----
    //
    // 这类文件不在 PE 资源里，ExtractIconEx 拿不到，shell 关联给的又是
    // 「图片文件」的通用图标 —— 必须自己解码。.ico 也一并交给 WIC：
    // 它会把多尺寸 ico 里最大的那一帧拿出来，比 ExtractIconEx 的 32px 强。
    if (isDecodableImageFile(filePath)) {
        ImagePixels image;
        if (decodeImageFile(filePath, &image)) {
            HICON icon = iconFromPixels(image);
            if (icon != nullptr) {
                return UniqueHIcon(icon);
            }
        }
        // 解码失败就往下走通用路径：至少给个「图片文件」关联图标，
        // 好过完全空白
    }

    // ---- 方式一：PE 模块直接从资源提取（exe/dll/…）----
    //
    // 注意顺序：这里必须排在 ImageList 之前。jumbo ImageList 对「图标
    // 资源里没有 256px 帧」的 exe 会返回退化的 256 画布装 48 小图
    // （实测 i_view64.exe 内容占比仅 3.5%，Bandicam 同样中招），
    // 铺到磁贴上就是一小点。PrivateExtractIconsW 则总能给出铺满画布
    // 的图标（见 extractScaledPeIcon 注释与验证/目录的 icon_probe 实测）。
    if (hasIconBearingExtension(filePath)) {
        HICON peIcon = extractScaledPeIcon(filePath, desired);
        if (peIcon != nullptr) {
            return UniqueHIcon(peIcon);
        }
    }

    // ---- 方式二：ImageList（文件夹/文档等非 PE 目标的大图标来源）----
    //
    // ExtractIconEx 只能拿到 PE 资源里的「默认」尺寸（通常 32px），
    // 一旦优先使用，62px/128px 的磁贴会把它放大成模糊噪点。
    HICON jumbo = iconFromSystemImageList(filePath, false, desired);
    if (jumbo != nullptr) {
        return UniqueHIcon(jumbo);
    }

    // ---- 方式三：ExtractIconEx（32px 兜底）----
    if (hasIconBearingExtension(filePath)) {
        HICON largeIcon = nullptr;
        HICON smallIcon = nullptr;
        const UINT extracted = ExtractIconExW(filePath.c_str(), 0,
                                             &largeIcon, &smallIcon, 1);
        if (extracted > 0 && largeIcon != nullptr) {
            if (smallIcon != nullptr) {
                DestroyIcon(smallIcon);
            }
            return UniqueHIcon(largeIcon);
        }
        if (smallIcon != nullptr) {
            DestroyIcon(smallIcon);
        }
    }

    // ---- 方式四：SHGetFileInfo 通用 ----
    HICON shellIcon = shellIconForPath(filePath, false);
    if (shellIcon != nullptr) {
        return UniqueHIcon(shellIcon);
    }

    return UniqueHIcon();
}

UniqueHIcon loadDefaultApplicationIcon(const IconSizeHint hint) {
    // 用 notepad.exe 作为「通用程序」的保险选择（系统必带）
    const wchar_t* fallback = L"C:\\Windows\\System32\\notepad.exe";
    UniqueHIcon icon = loadIconFromFile(fallback, hint);
    if (icon.valid()) {
        return icon;
    }
    // 再退一层：shell32.dll 里的信息图标
    return loadIconFromFile(L"C:\\Windows\\System32\\shell32.dll", hint);
}

UniqueHIcon loadItemIcon(const std::string& targetUtf8,
                         const std::string& customIconUtf8,
                         const bool isFolder,
                         const bool isUrl,
                         const IconSizeHint hint) {
    // ---- 1. 自定义图标优先 ----
    if (!customIconUtf8.empty()) {
        // item.icon 里存的是相对 data 目录的路径（"res\xx.png"），
        // 这里解析成绝对路径；老数据里的绝对路径原样通过
        const std::wstring customPath = resolveStoredIconPath(
            base::utf8ToWide(customIconUtf8));
        UniqueHIcon icon = loadIconFromFile(customPath, hint);
        if (icon.valid()) {
            return icon;
        }
        // 自定义图标路径无效时继续尝试 target，不直接失败
    }

    if (targetUtf8.empty()) {
        return loadDefaultApplicationIcon(hint);
    }

    const std::wstring target = base::utf8ToWide(targetUtf8);

    // ---- 2. URL：用默认浏览器图标 ----
    if (isUrl) {
        // 取 .html 的关联图标（大多数系统即浏览器图标）
        HICON icon = iconFromSystemImageList(L"index.html", false,
                                            pixelSizeForHint(hint));
        if (icon != nullptr) {
            return UniqueHIcon(icon);
        }
        HICON shellIcon = shellIconForPath(L"index.html", false);
        if (shellIcon != nullptr) {
            return UniqueHIcon(shellIcon);
        }
        return loadDefaultApplicationIcon(hint);
    }

    // ---- 3. 文件夹 ----
    if (isFolder) {
        const int desired = pixelSizeForHint(hint);
        // 泛型白页文件夹图标没有辨识度，交给上层按名称首字渲染
        if (!looksGeneric(target, true, desired)) {
            HICON icon = iconFromSystemImageList(target, true, desired);
            if (icon != nullptr) {
                return UniqueHIcon(icon);
            }
            HICON shellIcon = shellIconForPath(target, true);
            if (shellIcon != nullptr) {
                return UniqueHIcon(shellIcon);
            }
        }
        return UniqueHIcon();
    }

    // ---- 4. 普通文件/可执行文件 ----
    UniqueHIcon icon = loadIconFromFile(target, hint);
    if (icon.valid()) {
        return icon;
    }

    // 文件不存在时，用扩展名取关联图标（用户可能还没创建该文件）
    const int desired = pixelSizeForHint(hint);
    if (!looksGeneric(target, false, desired)) {
        HICON associatedIcon = iconFromSystemImageList(target, false, desired);
        if (associatedIcon != nullptr) {
            return UniqueHIcon(associatedIcon);
        }
    }

    // 全部失败：返回空，由 UI 层按名称首字兜底渲染
    return UniqueHIcon();
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
