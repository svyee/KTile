// KTile - 自定义图标仓库测试
//
// 覆盖「用户从编辑对话框选一张图 → 存成图标文件」这条链路：
//   1. 解码 → **按原图比例**缩放到最长边 128（不裁剪、不压扁）
//   2. 编码为 PNG
//   3. 落盘到 <data>\res，返回值是可移植的相对路径
//   4. 相对路径能解析回真实文件，且能被重新解码回同样比例
//
// 为什么不靠 UI 点一遍来验证：文件选择框是模态的系统对话框，
// 自动化点不动。把「导入」这件事单独测掉，UI 那边只需验证
// 「点了预览会调用它」——两者加起来覆盖完整。
//
#include "platform/win32/icon_store.h"

#include <Windows.h>
#include <objbase.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ktile::platform::win32;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// ---------------------------------------------------------------- 测试素材

// 手写一张 24bpp BMP：三条色带。
//   horizontal = false → 左红 / 中绿 / 右蓝（竖带）
//   horizontal = true  → 上红 / 中绿 / 下蓝（横带）
// 用 BMP 而不是 PNG 是为了不依赖任何编码器 —— 测试要能独立跑。
bool writeBandedBmp(const std::wstring& path, const int width,
                    const int height, const bool horizontal = false) {
    const int rowBytes = width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * height;
    const std::uint32_t fileSize = 14 + 40 + imageBytes;

    std::vector<std::uint8_t> file(static_cast<std::size_t>(fileSize), 0);

    // BITMAPFILEHEADER
    file[0] = 'B';
    file[1] = 'M';
    std::memcpy(&file[2], &fileSize, 4);
    const std::uint32_t pixelOffset = 54;
    std::memcpy(&file[10], &pixelOffset, 4);

    // BITMAPINFOHEADER
    const std::uint32_t headerSize = 40;
    std::memcpy(&file[14], &headerSize, 4);
    std::memcpy(&file[18], &width, 4);
    const std::int32_t signedHeight = height;
    std::memcpy(&file[22], &signedHeight, 4);
    const std::uint16_t planes = 1;
    const std::uint16_t bits = 24;
    std::memcpy(&file[26], &planes, 2);
    std::memcpy(&file[28], &bits, 2);
    std::memcpy(&file[34], &imageBytes, 4);

    // 像素（BMP 自下而上存 BGR）
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(pixelOffset) +
                static_cast<std::size_t>(y) * (rowBytes + padding) + x * 3;
            const int third = horizontal ? height / 3 : width / 3;
            // BMP 自下而上存：文件里 y=0 是屏幕上最底下一行，
            // 所以「横带」要按 height-1-y 判断段位。
            const int position = horizontal ? (height - 1 - y) : x;
            const int band = position < third
                                 ? 0
                                 : (position < third * 2 ? 1 : 2);
            const std::uint8_t red = (band == 0) ? 0xFF : 0;
            const std::uint8_t green = (band == 1) ? 0xFF : 0;
            const std::uint8_t blue = (band == 2) ? 0xFF : 0;
            file[index] = blue;
            file[index + 1] = green;
            file[index + 2] = red;
        }
    }

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(handle, file.data(), fileSize, &written, nullptr)
                        ? written == fileSize
                        : false;
    CloseHandle(handle);
    return ok;
}

void ensureDirectoryExists(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
}

void removeTree(const std::wstring& path) {
    // 测试用完就清掉，别在 build 目录里留垃圾
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = path + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = path + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                removeTree(child);
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data) != FALSE);
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

std::wstring executableDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path = buffer;
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

// ---------------------------------------------------------------- 用例

// 取像素（越界返回 0，便于断言失败时给出可读信息）
std::uint32_t pixelAt(const ImagePixels& pixels, const int x, const int y) {
    if (x < 0 || y < 0 || x >= pixels.width || y >= pixels.height) {
        return 0U;
    }
    return pixels.pixels[static_cast<std::size_t>(y) * pixels.width + x];
}

std::uint32_t redOf(const std::uint32_t p) { return (p >> 16) & 0xFFU; }
std::uint32_t greenOf(const std::uint32_t p) { return (p >> 8) & 0xFFU; }
std::uint32_t blueOf(const std::uint32_t p) { return p & 0xFFU; }
std::uint32_t alphaOf(const std::uint32_t p) { return (p >> 24) & 0xFFU; }

// 横图（2:1）→ 存成 128x64：**比例必须保住**，三条竖带一条都不能丢。
//
// 这是「让用户选的图铺满磁贴」的前提：只要比例还在，把图铺进同比例的
// 磁贴内框就能整张图完整可见；一旦在导入时裁成正方形，构图就永久丢了。
void testImportKeepsWideAspect() {
    const std::wstring root = executableDirectory() + L"\\icon_store_work";
    ensureDirectoryExists(root);

    const std::wstring sourcePath = root + L"\\banded.bmp";
    CHECK(writeBandedBmp(sourcePath, 200, 100));

    setIconStoreRoot(root);

    std::wstring error;
    const std::wstring stored = importCustomIcon(sourcePath, &error);
    CHECK(!stored.empty());
    if (stored.empty()) {
        std::printf("    error=%ls\n", error.c_str());
        removeTree(root);
        return;
    }
    // 存的是相对 data 目录的路径 → 便携
    CHECK(stored.rfind(L"res\\", 0) == 0);
    CHECK(stored.size() > 8);

    const std::wstring absolute = resolveStoredIconPath(stored);
    CHECK(absolute.rfind(root, 0) == 0);
    CHECK(GetFileAttributesW(absolute.c_str()) != INVALID_FILE_ATTRIBUTES);

    // 长边缩到 128、短边按比例 → 128x64（旧实现会硬缩成 128x128）
    ImagePixels pixels;
    CHECK(decodeImageFile(absolute, &pixels));
    CHECK(pixels.width == 128);
    CHECK(pixels.height == 64);
    if (!pixels.valid()) {
        removeTree(root);
        return;
    }

    // 不裁剪：源图 200x100 的竖带边界在 66 / 133，缩放后落在 42 / 85。
    // 若实现成「拉伸成正方形」或「居中裁成方形」，这三条不可能同时
    // 分别是红 / 绿 / 蓝。
    const std::uint32_t left = pixelAt(pixels, 6, 32);
    const std::uint32_t middle = pixelAt(pixels, 64, 32);
    const std::uint32_t right = pixelAt(pixels, 121, 32);

    CHECK(redOf(left) > 200 && greenOf(left) < 40 && blueOf(left) < 40);
    CHECK(greenOf(middle) > 200 && redOf(middle) < 40 && blueOf(middle) < 40);
    CHECK(blueOf(right) > 200 && redOf(right) < 40 && greenOf(right) < 40);

    // BMP 没有 alpha 通道 → 解码后应该是完全不透明
    CHECK(alphaOf(left) == 0xFFU);

    removeTree(root);
}

// 竖图（1:2）→ 存成 64x128：上 / 中 / 下三条横带全部保留。
//
// 这条直接对应「1x2 磁贴上整张图铺满」：源图最上和最下两行都还在，
// 说明导入没有把图裁掉任何一段。
void testImportKeepsTallAspect() {
    const std::wstring root = executableDirectory() + L"\\icon_store_work";
    ensureDirectoryExists(root);

    const std::wstring sourcePath = root + L"\\stacked.bmp";
    CHECK(writeBandedBmp(sourcePath, 100, 200, true));

    setIconStoreRoot(root);

    std::wstring error;
    const std::wstring stored = importCustomIcon(sourcePath, &error);
    CHECK(!stored.empty());
    if (stored.empty()) {
        std::printf("    error=%ls\n", error.c_str());
        removeTree(root);
        return;
    }

    ImagePixels pixels;
    CHECK(decodeImageFile(resolveStoredIconPath(stored), &pixels));
    CHECK(pixels.width == 64);
    CHECK(pixels.height == 128);
    if (!pixels.valid()) {
        removeTree(root);
        return;
    }

    const std::uint32_t top = pixelAt(pixels, 32, 8);
    const std::uint32_t center = pixelAt(pixels, 32, 64);
    const std::uint32_t bottom = pixelAt(pixels, 32, 120);

    CHECK(redOf(top) > 200 && greenOf(top) < 40 && blueOf(top) < 40);
    CHECK(greenOf(center) > 200 && redOf(center) < 40 &&
          blueOf(center) < 40);
    CHECK(blueOf(bottom) > 200 && redOf(bottom) < 40 && greenOf(bottom) < 40);
    // 上下两端的像素不同 → 源图的头尾都在（没被裁掉）
    CHECK(top != bottom);

    removeTree(root);
}

void testImportRejectsNonImage() {
    const std::wstring root = executableDirectory() + L"\\icon_store_work";
    ensureDirectoryExists(root);
    setIconStoreRoot(root);

    const std::wstring textPath = root + L"\\notimage.txt";
    HANDLE handle = CreateFileW(textPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }

    std::wstring error;
    CHECK(importCustomIcon(textPath, &error).empty());
    CHECK(!error.empty());

    // 不存在的文件同样要安全失败
    error.clear();
    CHECK(importCustomIcon(root + L"\\missing.png", &error).empty());
    CHECK(!error.empty());

    DeleteFileW(textPath.c_str());
    removeTree(root);
}

void testResolveStoredIconPath() {
    setIconStoreRoot(L"D:\\KTile\\data");
    // 相对路径 → 拼到 data 目录下
    CHECK(resolveStoredIconPath(L"res\\a.png") == L"D:\\KTile\\data\\res\\a.png");
    // 绝对路径（老数据 / 用户手填）→ 原样保留
    CHECK(resolveStoredIconPath(L"E:\\icons\\old.ico") == L"E:\\icons\\old.ico");

    // 结尾的反斜杠要被裁掉，避免出现 "data\\\\res"
    setIconStoreRoot(L"D:\\KTile\\data\\");
    CHECK(resolveStoredIconPath(L"res\\a.png") == L"D:\\KTile\\data\\res\\a.png");
}

void testManagedIconPath() {
    // 相对路径 = 本程序托管的用户图片（磁贴上要铺满整块磁贴）
    CHECK(isManagedIconPath(L"res\\a1b2c3d4.png"));
    CHECK(isManagedIconPath(L"res\\sub\\a.png"));
    // 绝对路径 / UNC = 老数据或用户手填 → 按系统图标处理（方框居中）
    CHECK(!isManagedIconPath(L"E:\\icons\\old.ico"));
    CHECK(!isManagedIconPath(L"\\\\server\\share\\a.png"));
    CHECK(!isManagedIconPath(L""));
}

void testPremultiplyBgra() {
    ImagePixels pixels;
    pixels.width = 3;
    pixels.height = 1;
    pixels.pixels = {0x80FF0000U, 0x00010203U, 0xFF123456U};

    const std::vector<std::uint32_t> out = premultiplyBgra(pixels);
    CHECK(out.size() == 3);
    if (out.size() != 3) {
        return;
    }
    // 半透明：RGB 乘 alpha/255（0xFF * 0x80 / 255 = 128），alpha 不动
    CHECK(alphaOf(out[0]) == 0x80U);
    CHECK(redOf(out[0]) == 128U && greenOf(out[0]) == 0U &&
          blueOf(out[0]) == 0U);
    // 全透明：RGB 清零（否则 AlphaBlend / D2D 会在边缘泛白）
    CHECK(out[1] == 0U);
    // 不透明：原样保留
    CHECK(out[2] == 0xFF123456U);
}

void testDecodableExtensions() {
    CHECK(isDecodableImageFile(L"C:\\a\\b.ico"));
    CHECK(isDecodableImageFile(L"C:\\a\\b.PNG"));
    CHECK(isDecodableImageFile(L"C:\\a\\b.JpG"));
    CHECK(isDecodableImageFile(L"C:\\a\\b.bmp"));
    CHECK(!isDecodableImageFile(L"C:\\a\\b.exe"));
    CHECK(!isDecodableImageFile(L"C:\\a\\b"));
}

}  // namespace

int main() {
    const HRESULT com = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    testDecodableExtensions();
    testResolveStoredIconPath();
    testManagedIconPath();
    testPremultiplyBgra();
    testImportKeepsWideAspect();
    testImportKeepsTallAspect();
    testImportRejectsNonImage();

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }

    std::printf("检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("===== 图标仓库: 全部通过 =====\n");
        return 0;
    }
    std::printf("===== 图标仓库: %d 项失败 =====\n", g_failures);
    return 1;
}
