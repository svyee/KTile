// KTile - Win32 平台层：图标提取实现
#include "platform/win32/icon_loader.h"

#include "base/text_convert.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/program_paths.h"

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

// --- 应用执行别名（AppExecLink）解引用
//
// 商店应用（终端、计算器、截图工具……）的入口是 WindowsApps 下 0 字节的
// 重解析点，既不是 PE 也没有图标资源，直接提图标只会得到「通用空白程序」。
// 重解析数据里存着包内真实 exe 的完整路径（第 2 个字符串）。
// 该结构没有公开定义：Version 之后就是 NULL 分隔的宽字符串，没有 StringCount，
// 个别系统版本多塞一个计数字段会让取值整体错位，所以解析后必须校验
// 「像绝对路径且文件真实存在」，不成立换备用偏移再试。符号链接一并处理。
constexpr DWORD kFsctlGetReparsePoint = 0x000900A8;
constexpr ULONG kReparseTagMountPoint = 0xA0000003;
constexpr ULONG kReparseTagSymlink = 0xA000000C;
constexpr ULONG kReparseTagAppExecLink = 0x8000001B;

// 绝对路径且真实存在的普通文件。WindowsApps 目录不许列举，
// 但按完整路径访问单个文件可以。
bool isAbsoluteExistingFile(const std::wstring& path) {
    if (path.size() < 3) {
        return false;
    }
    const bool driveAbsolute =
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool uncPath = path[0] == L'\\' && path[1] == L'\\';
    if (!driveAbsolute && !uncPath) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 按 NULL 切分重解析数据里的宽字符串，取第 index 个。
std::wstring reparseStringAt(const std::vector<unsigned char>& buffer,
                             const std::size_t dataBytes,
                             const std::size_t stringsOffset,
                             const std::size_t index) {
    if (dataBytes <= stringsOffset) {
        return std::wstring();
    }
    const std::size_t charCount =
        (dataBytes - stringsOffset) / sizeof(wchar_t);
    const std::wstring text(
        reinterpret_cast<const wchar_t*>(buffer.data() + stringsOffset),
        charCount);

    std::size_t current = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = position;
        while (end < text.size() && text[end] != L'\0') {
            ++end;
        }
        if (current == index) {
            return text.substr(position, end - position);
        }
        ++current;
        position = end + 1;
    }
    return std::wstring();
}

// 若是应用执行别名或符号链接，返回其指向的真实文件路径；否则返回空。
std::wstring resolveAliasTarget(const std::wstring& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        // OPEN_REPARSE_POINT：拿链接本身而不是顺着它走，
        // 否则 DeviceIoControl 会因为目标不可访问而失败。
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }

    std::vector<unsigned char> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        handle, kFsctlGetReparsePoint, nullptr, 0, buffer.data(),
        static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);
    if (ok == FALSE || returned < 12) {
        return std::wstring();
    }

    const ULONG tag = *reinterpret_cast<const ULONG*>(buffer.data());

    if (tag == kReparseTagAppExecLink) {
        // 第 2 个字符串就是包内 exe 的完整路径
        std::wstring executable = reparseStringAt(buffer, returned, 12, 2);
        if (isAbsoluteExistingFile(executable)) {
            return executable;
        }
        // 备用布局：Version 后多一个 StringCount
        executable = reparseStringAt(buffer, returned, 16, 2);
        if (isAbsoluteExistingFile(executable)) {
            return executable;
        }
        return std::wstring();
    }

    if (tag == kReparseTagSymlink || tag == kReparseTagMountPoint) {
        constexpr std::size_t kHeaderBytes = 8;
        if (returned < kHeaderBytes + 8) {
            return std::wstring();
        }
        const auto* fields =
            reinterpret_cast<const USHORT*>(buffer.data() + kHeaderBytes);
        const std::size_t substituteOffset = fields[0];
        const std::size_t substituteBytes = fields[1];
        // 符号链接多一个 ULONG Flags，路径缓冲区比挂载点晚 4 字节
        const std::size_t pathStart =
            kHeaderBytes + (tag == kReparseTagSymlink ? 12 : 8);
        if (pathStart + substituteOffset + substituteBytes > returned) {
            return std::wstring();
        }
        std::wstring target(substituteBytes / sizeof(wchar_t), L'\0');
        if (!target.empty()) {
            std::memcpy(&target[0],
                        buffer.data() + pathStart + substituteOffset,
                        substituteBytes);
        }
        // "\??\C:\..." → "C:\..."
        if (target.compare(0, 4, L"\\??\\") == 0) {
            target.erase(0, 4);
        }
        if (isAbsoluteExistingFile(target)) {
            return target;
        }
    }
    return std::wstring();
}

// 扩展名是否可当 PE 模块直接提取（exe/dll/ico/cpl/scr/ocx）
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

// 用 SHGetFileInfo 取图标，适合文档、文件夹、URL。
// 返回的 HICON 由调用方销毁。
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

// 从系统图标 ImageList 按索引取图，唯一能拿到 256px jumbo 的方式。
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

// 测量 HICON 非透明内容占画布的边长比；取不到像素返回 1.0，宁可放过。
// 用于识别 jumbo ImageList 的退化：exe 的图标资源里没有 256px 帧时，GetIcon
// 给的是「256 画布左上角贴一张 48 小图」（i_view64.exe、Bandicam 均如此）。
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

    // jumbo 退化兜底（判定见 iconContentRatio）。退回 48px 的 EXTRALARGE：
    // 放大发虚好过缩成一个小点。
    if (icon != nullptr && imageListId == SHIL_JUMBO &&
        iconContentRatio(icon) < 0.5) {
        DestroyIcon(icon);
        icon = iconFromImageListId(path, isFolder, SHIL_EXTRALARGE);
    }
    return icon;
}

// 直接从 PE 资源按请求尺寸提取图标。
// PrivateExtractIconsW 与资源管理器同源，会挑最接近请求尺寸的帧再缩放：
// i_view64.exe、Bandicam 能拿到真正的 256x256，而 jumbo ImageList 只给
// 256 画布装 48 小图，notepad/chrome 则两者一致。
// Windows 2000 起可用，Win7 无兼容问题。
HICON extractScaledPeIcon(const std::wstring& path, const int desiredSize) {
    HICON icon = nullptr;
    const UINT extracted = PrivateExtractIconsW(
        path.c_str(), 0, desiredSize, desiredSize, &icon, nullptr, 1, 0);
    if (extracted > 0 && icon != nullptr) {
        return icon;
    }
    return nullptr;
}

// 判断 Shell 给的是否「通用空白页」图标：文件不存在或无关联时 Shell 返回
// 白页，没有辨识度，不如退回按名称首字渲染。做法是统计非透明像素里
// 无彩色像素的占比，几乎全灰即判为泛型。
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
            // 不透明像素足够多、且其中绝大多数无彩色，判为泛型白页
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
// 掩码位图必须给，哪怕是全 0：32bpp 图标的透明度由 alpha 决定，
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

    // 别名 / 符号链接先解到真实文件：别名本身是 0 字节重解析点，没有图标可提。
    // 解不出来（普通文件、权限不足）就原样用传入路径。
    std::wstring path = resolveAliasTarget(filePath);
    if (path.empty()) {
        path = filePath;
    }

    const int desired = pixelSizeForHint(hint);

    // --- 方式零：位图文件（png / jpg / jpeg / bmp / gif / tif / ico）
    // 这类文件不在 PE 资源里，ExtractIconEx 拿不到，Shell 给的只是「图片文件」
    // 的通用图标，必须自己解码。ico 也交给 WIC：它取多尺寸里最大的一帧，
    // 比 ExtractIconEx 的 32px 强。
    if (isDecodableImageFile(path)) {
        ImagePixels image;
        if (decodeImageFile(path, &image)) {
            HICON icon = iconFromPixels(image);
            if (icon != nullptr) {
                return UniqueHIcon(icon);
            }
        }
        // 解码失败继续走通用路径，至少给个「图片文件」关联图标
    }

    // --- 方式一：PE 模块直接从资源提取（exe/dll/…）
    // 必须排在 ImageList 之前：jumbo 对「图标资源里没有 256px 帧」的 exe
    // 返回退化的 256 画布装 48 小图（i_view64.exe 内容占比 3.5%），
    // PrivateExtractIconsW 则总能给出铺满画布的图标。
    if (hasIconBearingExtension(path)) {
        HICON peIcon = extractScaledPeIcon(path, desired);
        if (peIcon != nullptr) {
            return UniqueHIcon(peIcon);
        }
    }

    // --- 方式二：ImageList，非 PE 目标（文件夹/文档）的大图标来源
    // ExtractIconEx 只能拿 PE 资源里的默认尺寸（通常 32px），
    // 优先用它会让 62px/128px 的磁贴放大成模糊噪点。
    HICON jumbo = iconFromSystemImageList(path, false, desired);
    if (jumbo != nullptr) {
        return UniqueHIcon(jumbo);
    }

    // --- 方式三：ExtractIconEx（32px 兜底）
    if (hasIconBearingExtension(path)) {
        HICON largeIcon = nullptr;
        HICON smallIcon = nullptr;
        const UINT extracted = ExtractIconExW(path.c_str(), 0,
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

    // --- 方式四：SHGetFileInfo 通用
    HICON shellIcon = shellIconForPath(path, false);
    if (shellIcon != nullptr) {
        return UniqueHIcon(shellIcon);
    }

    return UniqueHIcon();
}

UniqueHIcon loadDefaultApplicationIcon(const IconSizeHint hint) {
    // 用 notepad.exe 作「通用程序」保险选择（系统必带）
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
    // --- 自定义图标优先
    if (!customIconUtf8.empty()) {
        // item.icon 存的是相对 data 目录的路径（"res\xx.png"），这里解析成
        // 绝对路径；老数据里的绝对路径原样通过
        const std::wstring customPath = resolveStoredIconPath(
            base::utf8ToWide(customIconUtf8));
        UniqueHIcon icon = loadIconFromFile(customPath, hint);
        if (icon.valid()) {
            return icon;
        }
        // 自定义图标无效时继续尝试 target
    }

    if (targetUtf8.empty()) {
        return loadDefaultApplicationIcon(hint);
    }

    // 相对 exe 目录的路径（"tools\app.exe"）要先解析成绝对路径，
    // 否则按当前工作目录去找，磁贴变空白方块。
    const std::wstring target =
        resolveProgramRelativePath(base::utf8ToWide(targetUtf8));

    // --- URL：用默认浏览器图标
    if (isUrl) {
        // 取 .html 的关联图标，多数系统即浏览器图标
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

    // --- 文件夹
    if (isFolder) {
        const int desired = pixelSizeForHint(hint);
        // 泛型白页文件夹没有辨识度，交给上层按名称首字渲染
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

    // --- 普通文件 / 可执行文件
    UniqueHIcon icon = loadIconFromFile(target, hint);
    if (icon.valid()) {
        return icon;
    }

    // 文件可能还没创建，退到按扩展名取关联图标
    const int desired = pixelSizeForHint(hint);
    if (!looksGeneric(target, false, desired)) {
        HICON associatedIcon = iconFromSystemImageList(target, false, desired);
        if (associatedIcon != nullptr) {
            return UniqueHIcon(associatedIcon);
        }
    }

    // 全部失败，由 UI 层按名称首字兜底渲染
    return UniqueHIcon();
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
