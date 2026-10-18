#include "platform/win32/builtin_icon.h"

#include "core/lucide_icons.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/svg_path.h"
#include "platform/win32/unique_handles.h"

// commoncontrols.h 依赖 commctrl.h 里的 IMAGELISTDRAWPARAMS/IMAGEINFO，
// 必须先 include commctrl.h；shellapi.h 提供 SHGetFileInfoW。
#include <commctrl.h>
#include <commoncontrols.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// ---- 常量 ------------------------------------------------------------
//
// 图标画布 128x128；「蓝底白线框」是内置项的统一视觉。
// 蓝色取晨光皮肤的 accent（#2B6CE6）：这个蓝在深/浅两种主题的磁贴上
// 都有足够对比度，也和 Windows 系统图标体系不打架。
constexpr UINT kIconSize = 128;
constexpr float kBackdropInset = 16.0F;   // 蓝底到画布边的距离
constexpr float kBackdropRadius = 26.0F;  // 圆角
// Lucide 图形本身是 24x24：放大 3.5 倍 = 84 画布单位，居中后留 22 的边。
// 图形实际占 20/24（Lucide 的图元都内缩 2），画出来约 70px，落在 96px
// 的圆角底内，四周留白均匀。
constexpr float kGlyphBox = 84.0F;
constexpr float kGlyphScale = kGlyphBox / 24.0F;
constexpr float kGlyphOffset = (kIconSize - kGlyphBox) / 2.0F;
// Lucide 原版线宽 2（24 单位），按同一比例放大
constexpr float kStrokeWidth = 2.0F * kGlyphScale;

// 系统图标的内容盒：128 画布上占 96，四周留 16 透明边。
// 与 Lucide 图形（24 里画 20 → 视觉约 70/96 底）的重量对齐 ——
// 系统图标本来多是「铺满外框」的，不再内缩会显得比邻居胖一圈。
constexpr UINT kShellBox = 96;

// 蓝底 / 白线框（#2B6CE6）。D2D1::ColorF 的构造函数可静态初始化。
const D2D1::ColorF kBackdropColor(0.169F, 0.424F, 0.902F, 1.0F);
const D2D1::ColorF kGlyphColor(1.0F, 1.0F, 1.0F, 1.0F);

// ---- 小工具 -----------------------------------------------------------

bool createD2dFactory(ID2D1Factory** factory) {
    return SUCCEEDED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
        reinterpret_cast<void**>(factory)));
}

bool createWicFactory(IWICImagingFactory** factory) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      __uuidof(IWICImagingFactory),
                                      reinterpret_cast<void**>(factory)));
}

// 建目录（与 icon_store 内部的同名函数同语义的本地副本：
// icon_store 不想把这个内部细节暴露进头文件）
bool createDirectoryIfMissing(const std::wstring& directory) {
    if (directory.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }
    return CreateDirectoryW(directory.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// 把 32bpp BGRA（直通 alpha）像素编码成 PNG 写到磁盘。
// 与 icon_store::importCustomIcon 的保存段同一做法 —— 那边是文件进
// 文件出，这里像素已经在内存里，少一段解码/缩放。
bool writePng(IWICImagingFactory* factory,
              const std::vector<std::uint32_t>& pixels,
              const UINT width, const UINT height,
              const std::wstring& path) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(frame.put(), properties.put())) ||
        FAILED(frame->Initialize(properties.get()))) {
        return false;
    }
    if (FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) {
        return false;
    }

    // PNG 编码器要求预乘（PBGRA）。直通 alpha 转预乘后写入。
    std::vector<std::uint32_t> premultiplied(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const std::uint32_t pixel = pixels[i];
        const std::uint32_t alpha = (pixel >> 24) & 0xFFU;
        const std::uint32_t red = ((pixel >> 16) & 0xFFU) * alpha / 255U;
        const std::uint32_t green = ((pixel >> 8) & 0xFFU) * alpha / 255U;
        const std::uint32_t blue = (pixel & 0xFFU) * alpha / 255U;
        premultiplied[i] =
            (alpha << 24) | (red << 16) | (green << 8) | blue;
    }

    if (FAILED(frame->WritePixels(height, width * 4U,
                                  static_cast<UINT>(premultiplied.size()) * 4U,
                                  reinterpret_cast<BYTE*>(
                                      premultiplied.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    return true;
}

// ---- 系统图标：从真实 shell 对象取图 ----------------------------------
//
// 「系统位置」组（此电脑/回收站/控制面板/计算器…）直接显示系统自己的
// 图标 —— 用户要的就是「它本来长什么样」，计算器的图标就该是计算器。
// 代价是样式随系统版本变化，但这组本来就是"系统里已有的东西"，
// 跟着系统走反而更自然；功能命令（锁屏/音量/关机）仍用 Lucide 线框，
// 因为它们没有系统图标可言。

// 把表里的来源解析成 shell 认得的路径：
//   "shell:startup" → CSIDL_STARTUP 的真实目录
//   裸 exe 名（"calc.exe"）→ 完整路径（SearchPathW 能处理重定向与
//     App Paths；失败再拼 GetSystemDirectoryW —— 上一版裸名提取失败
//     就是因为它把 "calc.exe" 当成相对路径去解析了）
std::wstring resolveIconSource(const std::wstring& source) {
    if (source.empty()) {
        return std::wstring();
    }
    if (source.compare(0, 6, L"shell:") == 0) {
        // 目前只有启动目录一项，不值得写一个通用的 shell: 解析器
        wchar_t buffer[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0,
                                       buffer))) {
            return buffer;
        }
        return std::wstring();
    }
    if (source.find(L'\\') != std::wstring::npos ||
        source.find(L'/') != std::wstring::npos ||
        source.compare(0, 2, L"::") == 0) {
        return source;  // 已经是完整路径或命名空间路径
    }
    wchar_t resolved[MAX_PATH] = {};
    if (SearchPathW(nullptr, source.c_str(), nullptr, MAX_PATH, resolved,
                    nullptr) > 0) {
        return resolved;
    }
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring(systemDirectory) + L"\\" + source;
    }
    return source;
}

// 取系统图标，尺寸尽量大：
//   SHIL_JUMBO（256px，Vista+）→ SHIL_EXTRALARGE（48px）→ SHGFI_ICON 的 32px。
// 16/32px 放大到磁贴上会糊成一片，所以三级都要试。
HICON loadShellIcon(const std::wstring& source) {
    const std::wstring path = resolveIconSource(source);
    if (path.empty()) {
        return nullptr;
    }

    SHFILEINFOW info{};
    DWORD_PTR result = SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                                      SHGFI_SYSICONINDEX | SHGFI_ICON |
                                          SHGFI_LARGEICON);
    if (result == 0) {
        // 命名空间路径（::{CLSID}）在个别系统上不认字符串形式，改走 PIDL
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0,
                                         nullptr)) &&
            pidl != nullptr) {
            result = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info,
                                    sizeof(info), SHGFI_SYSICONINDEX |
                                                      SHGFI_ICON |
                                                      SHGFI_LARGEICON |
                                                      SHGFI_PIDL);
            ILFree(pidl);
        }
    }
    if (result == 0) {
        return nullptr;
    }

    HICON icon = nullptr;
    const int sizes[] = {SHIL_JUMBO, SHIL_EXTRALARGE};
    for (const int imageListSize : sizes) {
        if (icon != nullptr) {
            break;
        }
        IImageList* imageList = nullptr;
        if (SUCCEEDED(SHGetImageList(imageListSize, IID_IImageList,
                                     reinterpret_cast<void**>(&imageList))) &&
            imageList != nullptr) {
            imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
            imageList->Release();
        }
    }
    if (icon == nullptr) {
        icon = info.hIcon;
        info.hIcon = nullptr;  // 所有权转移，下面统一释放
    }
    if (info.hIcon != nullptr) {
        DestroyIcon(info.hIcon);
    }
    return icon;
}

// HICON → 128 画布上的直通 BGRA（图标缩进 96 的内容盒、居中、四周透明）。
// 背景透明是刻意的：磁贴自己会衬底，多画一层底色会跟主题打架。
bool renderShellIcon(const core::BuiltinItem& item,
                     std::vector<std::uint32_t>* pixels) {
    if (item.iconSource == nullptr) {
        return false;
    }
    HICON icon = loadShellIcon(item.iconSource);
    if (icon == nullptr) {
        return false;
    }

    ComPtr<IWICImagingFactory> wic;
    const bool hasWic = createWicFactory(wic.put());
    ComPtr<IWICBitmap> source;
    const HRESULT fromIcon = hasWic
                                 ? wic->CreateBitmapFromHICON(icon, source.put())
                                 : E_FAIL;
    DestroyIcon(icon);
    if (FAILED(fromIcon)) {
        return false;
    }

    // CreateBitmapFromHICON 出来的可能是 32bppBGRA/PBGRA，统一转 PBGRA 再缩放
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source.get(),
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(converter.get(), kShellBox, kShellBox,
                                  WICBitmapInterpolationModeFant))) {
        return false;
    }

    const UINT stride = kShellBox * 4U;
    std::vector<BYTE> buffer(stride * kShellBox);
    if (FAILED(scaler->CopyPixels(nullptr, stride,
                                  static_cast<UINT>(buffer.size()),
                                  buffer.data()))) {
        return false;
    }

    const UINT offset = (kIconSize - kShellBox) / 2;
    pixels->assign(static_cast<std::size_t>(kIconSize) * kIconSize, 0U);
    for (UINT y = 0; y < kShellBox; ++y) {
        for (UINT x = 0; x < kShellBox; ++x) {
            const BYTE* src = buffer.data() + y * stride + x * 4U;
            // PBGRA → 直通 BGRA：反预乘（与 renderLucideIcon 同一约定）
            const std::uint32_t alpha = src[3];
            std::uint32_t blue = src[0];
            std::uint32_t green = src[1];
            std::uint32_t red = src[2];
            if (alpha > 0) {
                blue = (std::min)(255U, blue * 255U / alpha);
                green = (std::min)(255U, green * 255U / alpha);
                red = (std::min)(255U, red * 255U / alpha);
            }
            (*pixels)[static_cast<std::size_t>(y + offset) * kIconSize +
                      (x + offset)] =
                (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }
    return true;
}

// ---- 图标渲染：蓝底 + Lucide 白线框 -----------------------------------

bool renderLucideIcon(const std::wstring& id,
                      std::vector<std::uint32_t>* pixels) {
    const wchar_t* markup = core::lucideBuiltinMarkup(id);
    if (markup == nullptr) {
        return false;
    }

    ComPtr<ID2D1Factory> factory;
    if (!createD2dFactory(factory.put())) {
        return false;
    }
    ComPtr<IWICImagingFactory> wic;
    if (!createWicFactory(wic.put())) {
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(kIconSize, kIconSize,
                                 GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnDemand, bitmap.put()))) {
        return false;
    }
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.get(), properties,
                                                   target.put()))) {
        return false;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0F));

    ComPtr<ID2D1SolidColorBrush> backdropBrush;
    ComPtr<ID2D1SolidColorBrush> glyphBrush;
    if (FAILED(target->CreateSolidColorBrush(kBackdropColor,
                                             backdropBrush.put())) ||
        FAILED(target->CreateSolidColorBrush(kGlyphColor,
                                             glyphBrush.put()))) {
        return false;
    }

    // 蓝底
    const D2D1_ROUNDED_RECT backdrop = D2D1::RoundedRect(
        D2D1::RectF(kBackdropInset, kBackdropInset, kIconSize - kBackdropInset,
                    kIconSize - kBackdropInset),
        kBackdropRadius, kBackdropRadius);
    target->FillRoundedRectangle(&backdrop, backdropBrush.get());

    // 线框符号
    SvgGlyph glyph;
    if (!buildSvgGlyph(factory.get(), markup, kGlyphScale, kGlyphOffset,
                       kGlyphOffset, &glyph)) {
        return false;
    }
    ComPtr<ID2D1StrokeStyle> strokeStyle = createRoundStrokeStyle(factory.get());
    target->DrawGeometry(glyph.strokeGeometry.get(), glyphBrush.get(),
                         kStrokeWidth, strokeStyle.get());
    // 少数图标（如画板的调色孔）带实心小点
    if (glyph.fillGeometry) {
        target->FillGeometry(glyph.fillGeometry.get(), glyphBrush.get(), nullptr);
    }

    if (FAILED(target->EndDraw())) {
        return false;
    }

    // PBGRA（预乘）→ BGRA（直通），与 decodeImageFile 的像素约定对齐，
    // 这样走 writePng 的预乘转换后仍是正确的原图。
    const UINT stride = kIconSize * 4U;
    std::vector<BYTE> buffer(stride * kIconSize);
    if (FAILED(bitmap->CopyPixels(nullptr, stride,
                                  static_cast<UINT>(buffer.size()),
                                  buffer.data()))) {
        return false;
    }
    pixels->resize(kIconSize * kIconSize);
    for (std::size_t i = 0; i < pixels->size(); ++i) {
        const BYTE* src = buffer.data() + i * 4;
        // PBGRA → 直通 BGRA：反预乘
        const std::uint32_t alpha = src[3];
        std::uint32_t blue = src[0];
        std::uint32_t green = src[1];
        std::uint32_t red = src[2];
        if (alpha > 0) {
            blue = (std::min)(255U, blue * 255U / alpha);
            green = (std::min)(255U, green * 255U / alpha);
            red = (std::min)(255U, red * 255U / alpha);
        }
        (*pixels)[i] = (static_cast<std::uint32_t>(alpha) << 24) |
                       (red << 16) | (green << 8) | blue;
    }
    return true;
}

}  // namespace

std::wstring ensureBuiltinIcon(const core::BuiltinItem& item,
                               std::wstring* error) {
    const auto fail = [error](const wchar_t* message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::wstring();
    };

    const std::wstring root = iconStoreRoot();
    if (root.empty()) {
        return fail(L"数据目录尚未初始化，无法保存图标。");
    }
    const std::wstring directory = iconStoreDirectory();

    // 存储值固定按 id：同一内置项重复添加/跨主题都复用同一份 PNG
    const std::wstring storedValue = core::builtinIconFileName(item.id);
    const std::wstring targetPath = root + L"\\" + storedValue;

    // 幂等：已经生成过就直接复用（内置图标视觉不跟主题走，
    // 第二次添加无需重画）
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(targetPath.c_str(), GetFileExInfoStandard,
                             &attributes) != FALSE) {
        return storedValue;
    }

    // 目录不存在时建一下（icon_store 里同名函数是私有的，
    // 这里保持同语义的本地副本）
    if (!createDirectoryIfMissing(directory)) {
        return fail(L"无法创建 data\\res 目录（可能被写保护）。");
    }

    std::vector<std::uint32_t> pixels;
    // 系统位置项取真实系统图标；取不到（老系统/异常 shell 状态）就退回
    // 同 id 的 Lucide 矢量图 —— 宁可样式不统一，也不能出现空白磁贴。
    const bool shellIcon = item.iconSource != nullptr &&
                           renderShellIcon(item, &pixels);
    if (!shellIcon && !renderLucideIcon(item.id, &pixels)) {
        return fail(L"内置图标渲染失败。");
    }

    ComPtr<IWICImagingFactory> factory;
    if (!createWicFactory(factory.put())) {
        return fail(L"图像组件不可用（WIC 初始化失败）。");
    }
    if (!writePng(factory.get(), pixels, kIconSize, kIconSize, targetPath)) {
        return fail(L"写入图标文件失败。");
    }

    if (error != nullptr) {
        error->clear();
    }
    return storedValue;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
