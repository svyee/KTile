// KTile - 构建辅助工具：把 icon.png 转成多尺寸 app.ico
//
// 用法：
//   gen_app_icon.exe <source.png> <output.ico>
//
// 为什么自己写：项目不引入任何第三方图像库（PIL/ImageMagick），
// 而 Windows 自带的 WIC 就能完成「解码 -> 高质量缩放 -> PNG 编码」，
// 只依赖系统 DLL，随用随编，不增加产品运行时体积。
//
// 产出格式：标准 ICO 容器，16/24/32/48/64/128/256 七个尺寸，每帧都是
// PNG 压缩（Vista 起支持，Win7 没问题）。256 及以上必须用 PNG，
// 小尺寸也用 PNG 可以省下大量体积、且缩放质量由 WIC 保证。
#include <Windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr UINT kSizes[] = {16, 24, 32, 48, 64, 128, 256};

struct Frame {
    UINT size = 0;
    std::vector<unsigned char> png;
};

void putWord(std::vector<unsigned char>& out, const unsigned value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void putDword(std::vector<unsigned char>& out, const unsigned value) {
    putWord(out, value & 0xFFFF);
    putWord(out, (value >> 16) & 0xFFFF);
}

// 用 WIC 把源帧缩放到指定尺寸并编码成 PNG 字节流
bool encodePngFrame(IWICImagingFactory* factory, IWICBitmapSource* source,
                    const UINT size, std::vector<unsigned char>* out) {
    IWICBitmapScaler* scaler = nullptr;
    HRESULT hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || scaler == nullptr) {
        return false;
    }
    hr = scaler->Initialize(source, size, size,
                            WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        scaler->Release();
        return false;
    }

    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || stream == nullptr) {
        scaler->Release();
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr) && encoder != nullptr) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    if (SUCCEEDED(hr) && encoder != nullptr) {
        hr = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        hr = frame->Initialize(properties);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        hr = frame->SetSize(size, size);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        hr = frame->WriteSource(scaler, nullptr);
    }
    if (SUCCEEDED(hr) && frame != nullptr) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr) && encoder != nullptr) {
        hr = encoder->Commit();
    }

    if (SUCCEEDED(hr)) {
        HGLOBAL memory = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &memory)) &&
            memory != nullptr) {
            const SIZE_T bytes = GlobalSize(memory);
            const void* data = GlobalLock(memory);
            if (data != nullptr) {
                const unsigned char* begin =
                    static_cast<const unsigned char*>(data);
                out->assign(begin, begin + bytes);
                GlobalUnlock(memory);
            } else {
                hr = E_FAIL;
            }
        }
    }

    if (frame != nullptr) {
        frame->Release();
    }
    if (properties != nullptr) {
        properties->Release();
    }
    if (encoder != nullptr) {
        encoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    scaler->Release();
    return SUCCEEDED(hr) && !out->empty();
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: gen_app_icon <source.png> <out.ico>\n");
        return 2;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr)) {
        std::fwprintf(stderr, L"CoInitializeEx failed: 0x%08X\n", comHr);
        return 1;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_IWICImagingFactory,
                                 reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        std::fwprintf(stderr, L"WIC factory failed: 0x%08X\n", hr);
        CoUninitialize();
        return 1;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(argv[1], nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    IWICBitmapFrameDecode* source = nullptr;
    if (SUCCEEDED(hr) && decoder != nullptr) {
        hr = decoder->GetFrame(0, &source);
    }
    if (FAILED(hr) || source == nullptr) {
        std::fwprintf(stderr, L"decode source failed: 0x%08X\n", hr);
        if (decoder != nullptr) {
            decoder->Release();
        }
        factory->Release();
        CoUninitialize();
        return 1;
    }

    std::vector<Frame> frames;
    bool ok = true;
    for (const UINT size : kSizes) {
        Frame frame;
        frame.size = size;
        if (!encodePngFrame(factory, source, size, &frame.png)) {
            std::fwprintf(stderr, L"encode %u failed\n", size);
            ok = false;
            break;
        }
        frames.push_back(std::move(frame));
    }

    source->Release();
    decoder->Release();
    factory->Release();
    CoUninitialize();

    if (!ok) {
        return 1;
    }

    // ---- 组装 ICO 容器 ----
    std::vector<unsigned char> file;
    putWord(file, 0);                        // reserved
    putWord(file, 1);                        // type = icon
    putWord(file, static_cast<unsigned>(frames.size()));

    unsigned offset = 6 + 16 * static_cast<unsigned>(frames.size());
    for (const Frame& frame : frames) {
        // 尺寸 256 在目录项里记 0
        file.push_back(static_cast<unsigned char>(frame.size >= 256
                                                      ? 0
                                                      : frame.size));
        file.push_back(static_cast<unsigned char>(frame.size >= 256
                                                      ? 0
                                                      : frame.size));
        file.push_back(0);                    // 调色板颜色数
        file.push_back(0);                    // reserved
        putWord(file, 1);                     // planes
        putWord(file, 32);                    // bit count
        putDword(file, static_cast<unsigned>(frame.png.size()));
        putDword(file, offset);
        offset += static_cast<unsigned>(frame.png.size());
    }
    for (const Frame& frame : frames) {
        file.insert(file.end(), frame.png.begin(), frame.png.end());
    }

    FILE* handle = nullptr;
    if (_wfopen_s(&handle, argv[2], L"wb") != 0 || handle == nullptr) {
        std::fwprintf(stderr, L"cannot write %s\n", argv[2]);
        return 1;
    }
    const std::size_t written =
        std::fwrite(file.data(), 1, file.size(), handle);
    std::fclose(handle);

    std::wprintf(L"%s: %zu bytes, %zu frames\n", argv[2], written,
                 frames.size());
    return (written == file.size()) ? 0 : 1;
}
