// 图标提取诊断探针：测量各提取途径返回 HICON 的真实画布尺寸与内容占比。
// 纯 ASCII 源码 + ASCII 输出（构建约定）。
#include <Windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

struct IconReport {
    int canvasW = 0;
    int canvasH = 0;
    int contentLeft = 0;
    int contentTop = 0;
    int contentW = 0;
    int contentH = 0;
    double coverage = 0.0;  // content / canvas
};

// 度量 HICON：画布尺寸 + 非透明内容的包围盒。
IconReport measure(HICON icon, const char* label) {
    IconReport r;
    if (icon == nullptr) {
        std::printf("[%s] icon = NULL\n", label);
        return r;
    }

    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) {
        std::printf("[%s] GetIconInfo failed\n", label);
        return r;
    }

    BITMAP bm{};
    if (ii.hbmColor != nullptr &&
        GetObjectW(ii.hbmColor, sizeof(bm), &bm) != 0) {
        r.canvasW = bm.bmWidth;
        r.canvasH = bm.bmHeight;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = r.canvasW;
        bmi.bmiHeader.biHeight = -r.canvasH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(r.canvasW) *
            static_cast<std::size_t>(r.canvasH));
        HDC screen = GetDC(nullptr);
        const int ok = GetDIBits(screen, ii.hbmColor, 0,
                                 static_cast<UINT>(r.canvasH),
                                 pixels.data(), &bmi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);

        if (ok != 0) {
            int minX = r.canvasW, minY = r.canvasH;
            int maxX = -1, maxY = -1;
            for (int y = 0; y < r.canvasH; ++y) {
                for (int x = 0; x < r.canvasW; ++x) {
                    const std::uint32_t p =
                        pixels[static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(r.canvasW) +
                               static_cast<std::size_t>(x)];
                    if ((p >> 24) >= 16) {  // 非透明
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (x > maxX) maxX = x;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            if (maxX >= 0) {
                r.contentLeft = minX;
                r.contentTop = minY;
                r.contentW = maxX - minX + 1;
                r.contentH = maxY - minY + 1;
                r.coverage =
                    static_cast<double>(r.contentW * r.contentH) /
                    static_cast<double>(r.canvasW * r.canvasH);
            }
        }
    }
    if (ii.hbmColor != nullptr) DeleteObject(ii.hbmColor);
    if (ii.hbmMask != nullptr) DeleteObject(ii.hbmMask);

    std::printf("[%s] canvas=%dx%d content=(%d,%d)+%dx%d coverage=%.3f\n",
                label, r.canvasW, r.canvasH, r.contentLeft, r.contentTop,
                r.contentW, r.contentH, r.coverage);
    return r;
}

// 途径 A：SHGetImageList(SHIL_JUMBO) + SHGFI_USEFILEATTRIBUTES（现行代码）
HICON viaJumboUseFileAttributes(const wchar_t* path) {
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                        SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) {
        std::printf("[A] SHGetFileInfo failed\n");
        return nullptr;
    }
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                              reinterpret_cast<void**>(&list))) ||
        list == nullptr) {
        std::printf("[A] SHGetImageList failed\n");
        return nullptr;
    }
    HICON icon = nullptr;
    list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    list->Release();
    std::printf("[A] sysIndex=%d\n", info.iIcon);
    return icon;
}

// 途径 A2：SHGetImageList(SHIL_JUMBO)，不带 USEFILEATTRIBUTES（真实文件）
HICON viaJumboRealFile(const wchar_t* path) {
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path, 0, &info, sizeof(info),
                        SHGFI_SYSICONINDEX)) {
        std::printf("[A2] SHGetFileInfo failed\n");
        return nullptr;
    }
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                              reinterpret_cast<void**>(&list))) ||
        list == nullptr) {
        std::printf("[A2] SHGetImageList failed\n");
        return nullptr;
    }
    HICON icon = nullptr;
    list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    list->Release();
    std::printf("[A2] sysIndex=%d\n", info.iIcon);
    return icon;
}

// 途径 A3：EXTRALARGE（48px）
HICON viaExtraLarge(const wchar_t* path) {
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_SYSICONINDEX)) {
        return nullptr;
    }
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(SHIL_EXTRALARGE, IID_IImageList,
                              reinterpret_cast<void**>(&list))) ||
        list == nullptr) {
        return nullptr;
    }
    HICON icon = nullptr;
    list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
    list->Release();
    return icon;
}

// 途径 B：ExtractIconExW
HICON viaExtractIconEx(const wchar_t* path) {
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    const UINT n = ExtractIconExW(path, 0, &largeIcon, &smallIcon, 1);
    std::printf("[B] ExtractIconEx extracted=%u\n", n);
    if (smallIcon != nullptr) DestroyIcon(smallIcon);
    return (n > 0) ? largeIcon : nullptr;
}

// 途径 C：PrivateExtractIconsW 请求 256
HICON viaPrivateExtract(const wchar_t* path) {
    HICON icon = nullptr;
    const UINT n = PrivateExtractIconsW(path, 0, 256, 256, &icon, nullptr, 1,
                                        0);
    std::printf("[C] PrivateExtractIcons(256) got=%u\n", n);
    return (n > 0) ? icon : nullptr;
}

// 把 HICON 的颜色位图存成 32bpp BMP（供人工查看）
void dumpBmp(HICON icon, const wchar_t* file) {
    if (icon == nullptr) return;
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return;
    BITMAP bm{};
    if (ii.hbmColor == nullptr ||
        GetObjectW(ii.hbmColor, sizeof(bm), &bm) == 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return;
    }
    const int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, ii.hbmColor, 0, static_cast<UINT>(h), pixels.data(),
              &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);

    // 预乘转直通不便看图，直接把 alpha 抬到 255 便于观察内容
    for (std::uint32_t& p : pixels) {
        p |= 0xFF000000U;
    }

    const std::size_t dataSize =
        pixels.size() * sizeof(std::uint32_t);
    const std::size_t offset =
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    std::vector<std::uint8_t> file2(offset + dataSize, 0);
    BITMAPFILEHEADER* fh =
        reinterpret_cast<BITMAPFILEHEADER*>(file2.data());
    fh->bfType = 0x4D42;  // 'BM'
    fh->bfSize = static_cast<DWORD>(file2.size());
    fh->bfOffBits = static_cast<DWORD>(offset);
    BITMAPINFOHEADER* ih = reinterpret_cast<BITMAPINFOHEADER*>(
        file2.data() + sizeof(BITMAPFILEHEADER));
    ih->biSize = sizeof(BITMAPINFOHEADER);
    ih->biWidth = w;
    ih->biHeight = -h;
    ih->biPlanes = 1;
    ih->biBitCount = 32;
    ih->biCompression = BI_RGB;
    std::memcpy(file2.data() + offset, pixels.data(), dataSize);

    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(f, file2.data(), static_cast<DWORD>(file2.size()), &written,
              nullptr);
    CloseHandle(f);

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: icon_probe <exe-path>\n");
        return 1;
    }
    const int wlen = MultiByteToWideChar(
        CP_ACP, 0, argv[1], -1, nullptr, 0);
    std::vector<wchar_t> wideBuf(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wideBuf.data(), wlen);
    std::wstring path(wideBuf.data());

    std::printf("=== target: %s ===\n", argv[1]);

    const wchar_t* p = path.c_str();

    HICON a = viaJumboUseFileAttributes(p);
    measure(a, "A jumbo+USEFILEATTRIBUTES");
    dumpBmp(a, L"probe_A_jumbo_usefileattr.bmp");

    HICON a2 = viaJumboRealFile(p);
    measure(a2, "A2 jumbo+realfile");
    dumpBmp(a2, L"probe_A2_jumbo_realfile.bmp");

    HICON a3 = viaExtraLarge(p);
    measure(a3, "A3 extralarge48+realfile");

    HICON b = viaExtractIconEx(p);
    measure(b, "B ExtractIconEx");

    HICON c = viaPrivateExtract(p);
    measure(c, "C PrivateExtract(256)");
    dumpBmp(c, L"probe_C_private256.bmp");

    if (a) DestroyIcon(a);
    if (a2) DestroyIcon(a2);
    if (a3) DestroyIcon(a3);
    if (b) DestroyIcon(b);
    if (c) DestroyIcon(c);
    return 0;
}
