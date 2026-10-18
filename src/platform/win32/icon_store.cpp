// KTile - Win32 平台层：自定义图标仓库实现
#include "platform/win32/icon_store.h"

#include <Shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 仓库里图标的长边上限（磁贴最大边长）
constexpr int kStoredIconMaxSize = 128;

// 仓库子目录名（<data>\res）
constexpr const wchar_t* kStoreSubdirectory = L"res";

// 仓库根目录。<exe>\data 或降级后的 %LOCALAPPDATA%\KTile\data。
// 进程内一次性设置，之后只读 —— 不需要加锁。
std::wstring g_iconStoreRoot;

// 去掉结尾的分隔符，避免拼出 "data\\\res"
std::wstring withoutTrailingSeparator(std::wstring path) {
    while (!path.empty() &&
           (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring lowerExtensionOf(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return std::wstring();
    }
    std::wstring extension = path.substr(dot);
    for (wchar_t& ch : extension) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return extension;
}

// 64 位 FNV-1a。把「完整路径 + 文件大小 + 修改时间」一起揉进名字：
// 换一张图（哪怕路径没变）会得到不同的文件名，天然避开陈旧图标；
// 同一张图重复导入则命中同一个名字，直接覆盖，不会堆垃圾。
std::wstring iconFileNameFor(const std::wstring& fullPath,
                             const std::uint64_t fileSize,
                             const std::uint64_t lastWrite) {
    std::uint64_t hash = 1469598103934665603ULL;
    const std::uint64_t prime = 1099511628211ULL;

    auto mix = [&hash, prime](const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xFFULL;
            hash *= prime;
        }
    };

    for (const wchar_t ch : fullPath) {
        wchar_t lower = ch;
        if (lower >= L'A' && lower <= L'Z') {
            lower = static_cast<wchar_t>(lower - L'A' + L'a');
        }
        // 按两个字节混入，中文路径不会被低字节吞掉
        mix(static_cast<std::uint64_t>(lower & 0xFF));
        mix(static_cast<std::uint64_t>((lower >> 8) & 0xFF));
    }
    mix(fileSize);
    mix(lastWrite);

    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%016llx.png", static_cast<unsigned long long>(hash));
    return buffer;
}

// 取输入的绝对路径（失败时退化为原样），用于生成稳定哈希
std::wstring absolutePathOf(const std::wstring& path) {
    wchar_t buffer[4096] = {};
    const DWORD length =
        GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)),
                         buffer, nullptr);
    if (length == 0 || length >= std::size(buffer)) {
        return path;
    }
    return buffer;
}

bool createDirectoryIfMissing(const std::wstring& directory) {
    if (directory.empty()) {
        return false;
    }
    if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if (CreateDirectoryW(directory.c_str(), nullptr) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---------------------------------------------------------------- WIC 助手

bool createWicFactory(ComPtr<IWICImagingFactory>* factory) {
    return SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory->put())));
}

// 多帧文件（多尺寸 .ico）取面积最大的一帧 —— 源图越大，缩放后越清晰
bool selectLargestFrame(IWICBitmapDecoder* decoder,
                        IWICBitmapFrameDecode** outFrame) {
    UINT frameCount = 0;
    if (decoder == nullptr || outFrame == nullptr ||
        FAILED(decoder->GetFrameCount(&frameCount)) || frameCount == 0) {
        return false;
    }

    UINT best = 0;
    std::uint64_t bestArea = 0;
    for (UINT index = 0; index < frameCount; ++index) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(index, frame.put()))) {
            continue;
        }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(frame->GetSize(&width, &height))) {
            continue;
        }
        const std::uint64_t area =
            static_cast<std::uint64_t>(width) * height;
        if (area > bestArea) {
            bestArea = area;
            best = index;
        }
    }
    return SUCCEEDED(decoder->GetFrame(best, outFrame));
}

// 任意像素格式 → 32bpp BGRA → 拷进 ImagePixels
bool copyAsBgra(IWICImagingFactory* factory, IWICBitmapSource* source,
                ImagePixels* out) {
    if (factory == nullptr || source == nullptr || out == nullptr) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) {
        return false;
    }
    // 32bppBGRA（非预乘）—— 与 HICON 的 alpha 语义一致
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
        height == 0) {
        return false;
    }

    out->width = static_cast<int>(width);
    out->height = static_cast<int>(height);
    out->pixels.assign(static_cast<std::size_t>(width) * height, 0U);

    const UINT stride = width * 4U;
    if (FAILED(converter->CopyPixels(
            nullptr, stride, stride * height,
            reinterpret_cast<BYTE*>(out->pixels.data())))) {
        out->width = 0;
        out->height = 0;
        out->pixels.clear();
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 仓库路径

void setIconStoreRoot(std::wstring dataDirectory) {
    g_iconStoreRoot = withoutTrailingSeparator(std::move(dataDirectory));
}

std::wstring iconStoreRoot() { return g_iconStoreRoot; }

std::wstring iconStoreDirectory() {
    if (g_iconStoreRoot.empty()) {
        return std::wstring();
    }
    return g_iconStoreRoot + L"\\" + kStoreSubdirectory;
}

std::wstring resolveStoredIconPath(const std::wstring& storedPath) {
    if (storedPath.empty()) {
        return std::wstring();
    }
    // PathIsRelativeW 对 "C:\..." / "\\\\server\\share" 返回 FALSE
    if (PathIsRelativeW(storedPath.c_str()) == FALSE) {
        return storedPath;
    }
    if (g_iconStoreRoot.empty()) {
        return storedPath;  // 仓库未知：原样交给上层尝试
    }
    return g_iconStoreRoot + L"\\" + storedPath;
}

bool isManagedIconPath(const std::wstring& storedPath) {
    // 空 = 没设自定义图标；绝对路径 = 老数据 / 用户手填，按系统图标处理
    return !storedPath.empty() && PathIsRelativeW(storedPath.c_str()) != FALSE;
}

bool isDecodableImageFile(const std::wstring& path) {
    const std::wstring extension = lowerExtensionOf(path);
    if (extension.empty()) {
        return false;
    }
    return extension == L".ico" || extension == L".png" ||
           extension == L".jpg" || extension == L".jpeg" ||
           extension == L".bmp" || extension == L".gif" ||
           extension == L".tif" || extension == L".tiff";
}

// ---------------------------------------------------------------- 解码

bool decodeImageFile(const std::wstring& path, ImagePixels* out) {
    if (out == nullptr || path.empty()) {
        return false;
    }
    out->width = 0;
    out->height = 0;
    out->pixels.clear();

    ComPtr<IWICImagingFactory> factory;
    if (!createWicFactory(&factory)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            decoder.put()))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!selectLargestFrame(decoder.get(), frame.put())) {
        return false;
    }
    return copyAsBgra(factory.get(), frame.get(), out);
}

bool decodeImageFileCover(const std::wstring& path, const std::uint32_t targetW,
                          const std::uint32_t targetH, ImagePixels* out) {
    if (out == nullptr || path.empty() || targetW == 0 || targetH == 0) {
        return false;
    }
    out->width = 0;
    out->height = 0;
    out->pixels.clear();

    ComPtr<IWICImagingFactory> factory;
    if (!createWicFactory(&factory)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            decoder.put()))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!selectLargestFrame(decoder.get(), frame.put())) {
        return false;
    }

    // 先转成 BGRA（复用同一格式约定），量出源尺寸再决定缩放比
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) {
        return false;
    }
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(converter->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0) {
        return false;
    }

    // cover：需要的缩放比 = 两方向较大者。
    //   < 1 → 源图比目标大，缩到「刚好盖住」（省内存的整个意义所在）
    //   ≥ 1 → 源图本来就不够大，保持原尺寸（不放大，交给绘制端拉齐）
    const double scaleX = static_cast<double>(targetW) / sourceWidth;
    const double scaleY = static_cast<double>(targetH) / sourceHeight;
    const double coverScale = (std::max)(scaleX, scaleY);
    UINT drawWidth = sourceWidth;
    UINT drawHeight = sourceHeight;
    IWICBitmapSource* drawSource = converter.get();
    ComPtr<IWICBitmapScaler> scaler;
    if (coverScale < 0.999999) {
        drawWidth = static_cast<UINT>(sourceWidth * coverScale + 0.5);
        drawHeight = static_cast<UINT>(sourceHeight * coverScale + 0.5);
        if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
            FAILED(scaler->Initialize(converter.get(), drawWidth, drawHeight,
                                      WICBitmapInterpolationModeFant))) {
            return false;
        }
        drawSource = scaler.get();
    }

    out->width = static_cast<int>(drawWidth);
    out->height = static_cast<int>(drawHeight);
    out->pixels.assign(static_cast<std::size_t>(drawWidth) * drawHeight, 0U);

    const UINT stride = drawWidth * 4U;
    if (FAILED(drawSource->CopyPixels(
            nullptr, stride, stride * drawHeight,
            reinterpret_cast<BYTE*>(out->pixels.data())))) {
        out->width = 0;
        out->height = 0;
        out->pixels.clear();
        return false;
    }
    return true;
}

bool decodeIconPixels(const HICON icon, ImagePixels* out) {
    if (out == nullptr || icon == nullptr) {
        return false;
    }
    out->width = 0;
    out->height = 0;
    out->pixels.clear();

    ComPtr<IWICImagingFactory> factory;
    if (!createWicFactory(&factory)) {
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHICON(icon, bitmap.put()))) {
        return false;
    }
    return copyAsBgra(factory.get(), bitmap.get(), out);
}

std::vector<std::uint32_t> premultiplyBgra(const ImagePixels& pixels) {
    std::vector<std::uint32_t> out(pixels.pixels.size(), 0U);
    for (std::size_t index = 0; index < pixels.pixels.size(); ++index) {
        const std::uint32_t source = pixels.pixels[index];
        const std::uint32_t alpha = (source >> 24) & 0xFFU;
        if (alpha == 0U) {
            out[index] = 0U;  // 全透明像素的 RGB 无意义，清零更安全
            continue;
        }
        if (alpha == 0xFFU) {
            out[index] = source;
            continue;
        }
        const std::uint32_t red = ((source >> 16) & 0xFFU) * alpha / 255U;
        const std::uint32_t green = ((source >> 8) & 0xFFU) * alpha / 255U;
        const std::uint32_t blue = (source & 0xFFU) * alpha / 255U;
        out[index] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
    return out;
}

// ---------------------------------------------------------------- 导入

std::wstring importCustomIcon(const std::wstring& sourcePath,
                              std::wstring* error) {
    const auto fail = [error](const wchar_t* message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::wstring();
    };

    if (sourcePath.empty()) {
        return fail(L"没有选择图片文件。");
    }
    if (g_iconStoreRoot.empty()) {
        return fail(L"数据目录尚未初始化，无法保存图标。");
    }
    if (!isDecodableImageFile(sourcePath)) {
        return fail(L"仅支持 ico / png / jpg / bmp 图片。");
    }

    const std::wstring directory = iconStoreDirectory();
    if (!createDirectoryIfMissing(directory)) {
        return fail(L"无法创建 data\\res 目录（可能被写保护）。");
    }

    ComPtr<IWICImagingFactory> factory;
    if (!createWicFactory(&factory)) {
        return fail(L"图像解码组件不可用（WIC 初始化失败）。");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            sourcePath.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, decoder.put()))) {
        return fail(L"无法打开该图片（格式不支持或文件被占用）。");
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!selectLargestFrame(decoder.get(), frame.put())) {
        return fail(L"图片里没有可用的图像帧。");
    }

    // ① 统一转成 32bppPBGRA：Fant 插值只支持 32bpp 输入，
    //    而且这一步同时把调色板/灰度等格式拉平，后续裁剪缩放都不必再判断。
    ComPtr<IWICFormatConverter> toBgra;
    if (FAILED(factory->CreateFormatConverter(toBgra.put())) ||
        FAILED(toBgra->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom))) {
        return fail(L"图片像素格式无法转换。");
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(toBgra->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0) {
        return fail(L"无法读取图片尺寸。");
    }

    // ② 按原图比例缩放到「长边 = 128」——**不裁剪**。
    //
    // 横图存成 128xN、竖图存成 Nx128，原始构图完整保留。渲染时把这张图
    // 铺满磁贴内框，比例对得上（竖图配 1x2 磁贴）就是「整张图填满磁贴」。
    // 若在这里裁成正方形，用户选的构图就永久丢了 —— 预览里也换不回来。
    UINT targetWidth = 0;
    UINT targetHeight = 0;
    if (sourceWidth >= sourceHeight) {
        targetWidth = static_cast<UINT>(kStoredIconMaxSize);
        targetHeight = static_cast<UINT>(
            (static_cast<std::uint64_t>(sourceHeight) * kStoredIconMaxSize +
             sourceWidth / 2U) /
            sourceWidth);
    } else {
        targetHeight = static_cast<UINT>(kStoredIconMaxSize);
        targetWidth = static_cast<UINT>(
            (static_cast<std::uint64_t>(sourceWidth) * kStoredIconMaxSize +
             sourceHeight / 2U) /
            sourceHeight);
    }
    // 极端长宽比（例如 1000x3）四舍五入后可能落到 0 —— 至少留 1 像素
    targetWidth = (std::max)(1U, targetWidth);
    targetHeight = (std::max)(1U, targetHeight);

    // ③ Fant 高质量缩放（避免锯齿）
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(toBgra.get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeFant))) {
        return fail(L"图片缩放失败。");
    }

    // ④ PNG 编码器按 BGRA 写入：PBGRA → BGRA 再交回去，
    //    避免依赖编码器内部的隐式预乘/反预乘行为
    ComPtr<IWICFormatConverter> toStraight;
    if (FAILED(factory->CreateFormatConverter(toStraight.put())) ||
        FAILED(toStraight->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom))) {
        return fail(L"图标编码准备失败。");
    }

    // ⑤ 落到 data\res\<hash>.png
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    std::uint64_t fileSize = 0;
    std::uint64_t lastWrite = 0;
    if (GetFileAttributesExW(sourcePath.c_str(), GetFileExInfoStandard,
                             &attributes) != FALSE) {
        fileSize = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
                   attributes.nFileSizeLow;
        lastWrite =
            (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime)
             << 32) |
            attributes.ftLastWriteTime.dwLowDateTime;
    }

    const std::wstring fileName =
        iconFileNameFor(absolutePathOf(sourcePath), fileSize, lastWrite);
    const std::wstring targetPath = directory + L"\\" + fileName;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(targetPath.c_str(),
                                              GENERIC_WRITE))) {
        return fail(L"无法写入 data\\res（文件可能被占用）。");
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        return fail(L"PNG 编码器初始化失败。");
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(frameEncode.put(), properties.put())) ||
        FAILED(frameEncode->Initialize(properties.get()))) {
        return fail(L"无法创建 PNG 帧。");
    }

    if (FAILED(frameEncode->SetSize(targetWidth, targetHeight))) {
        return fail(L"无法设置图标尺寸。");
    }
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frameEncode->SetPixelFormat(&pixelFormat))) {
        return fail(L"PNG 像素格式不被支持。");
    }
    if (FAILED(frameEncode->WriteSource(toStraight.get(), nullptr)) ||
        FAILED(frameEncode->Commit()) || FAILED(encoder->Commit())) {
        return fail(L"写入图标文件失败。");
    }

    if (error != nullptr) {
        error->clear();
    }
    return std::wstring(kStoreSubdirectory) + L"\\" + fileName;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
