// KTile - OLE 拖放数据解析测试
//
// 测什么：从 IDataObject 里把「拖过来的东西」抠出来的这套逻辑。
//
// 为什么要用伪造的 IDataObject 而不是真的拖一次：
//   真实的拖放需要用户按住鼠标从资源管理器拖过来 —— 无法在自动化里稳定
//   复现（也正因如此，DragEnter/Drop 的「管道」由 Windows 负责，我们只
//   该为自己写的那部分负责：格式识别 + HGLOBAL 解析）。
//   这里用真的 GlobalAlloc + 真的 DROPFILES 内存布局喂进去，
//   所以被测的 DragQueryFileW 解析路径与线上运行时完全是同一条。
//
#include "platform/win32/drop_target.h"

#include <Windows.h>
#include <objidl.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ktile::platform::win32;

static int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            wprintf(L"FAIL line %d: %s\n", __LINE__, L#cond);    \
            ++g_failures;                                        \
        }                                                        \
    } while (false)

// ---------------------------------------------------------------- 伪造数据对象

// 一个只实现「取数据 / 问有没有」两个方法的内存版 IDataObject。
//
// 每次 GetData 都现场造一份新的 HGLOBAL 并把所有权交出去 —— 这与真实
// 实现的契约一致（调用方负责 ReleaseStgMedium），也让同一份测试可以在
// 「先问有没有，再取数据」的调用序列下工作。
class FakeDataObject final : public IDataObject {
public:
    // 待提供的格式 → 内容
    void setWideText(const CLIPFORMAT format, const std::wstring& text) {
        wideTexts_.push_back(Entry{format, text, {}});
    }

    void setPaths(const CLIPFORMAT format, const std::vector<std::wstring>& paths) {
        paths_.push_back(Entry{format, std::wstring(), paths});
    }

    // ---- IUnknown ----
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) noexcept {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppvObject = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() noexcept {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG __stdcall Release() noexcept {
        const LONG count = InterlockedDecrement(&refCount_);
        return count <= 0 ? 0 : static_cast<ULONG>(count);
    }

    // ---- IDataObject ----
    HRESULT __stdcall GetData(FORMATETC* format, STGMEDIUM* medium) noexcept {
        if (format == nullptr || medium == nullptr) {
            return E_INVALIDARG;
        }
        ZeroMemory(medium, sizeof(STGMEDIUM));

        for (const Entry& entry : paths_) {
            if (entry.format != format->cfFormat) {
                continue;
            }
            HGLOBAL handle = buildDropFiles(entry.paths);
            if (handle == nullptr) {
                return E_OUTOFMEMORY;
            }
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = handle;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }

        for (const Entry& entry : wideTexts_) {
            if (entry.format != format->cfFormat) {
                continue;
            }
            const std::size_t bytes =
                (entry.text.size() + 1) * sizeof(wchar_t);
            HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (handle == nullptr) {
                return E_OUTOFMEMORY;
            }
            void* locked = GlobalLock(handle);
            if (locked == nullptr) {
                GlobalFree(handle);
                return E_UNEXPECTED;
            }
            std::memcpy(locked, entry.text.c_str(), bytes);
            GlobalUnlock(handle);

            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = handle;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT __stdcall GetDataHere(FORMATETC*, STGMEDIUM*) noexcept {
        return E_NOTIMPL;
    }

    HRESULT __stdcall QueryGetData(FORMATETC* format) noexcept {
        if (format == nullptr) {
            return E_INVALIDARG;
        }
        for (const Entry& entry : paths_) {
            if (entry.format == format->cfFormat) {
                return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK
                                                            : DV_E_TYMED;
            }
        }
        for (const Entry& entry : wideTexts_) {
            if (entry.format == format->cfFormat) {
                return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK
                                                            : DV_E_TYMED;
            }
        }
        return DV_E_FORMATETC;
    }

    HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) noexcept {
        return E_NOTIMPL;
    }
    HRESULT __stdcall SetData(FORMATETC*, STGMEDIUM*, BOOL) noexcept {
        return E_NOTIMPL;
    }
    HRESULT __stdcall EnumFormatEtc(DWORD, IEnumFORMATETC**) noexcept {
        return E_NOTIMPL;
    }
    HRESULT __stdcall DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) noexcept {
        return E_NOTIMPL;
    }
    HRESULT __stdcall DUnadvise(DWORD) noexcept { return E_NOTIMPL; }
    HRESULT __stdcall EnumDAdvise(IEnumSTATDATA**) noexcept {
        return E_NOTIMPL;
    }

private:
    struct Entry {
        CLIPFORMAT format = 0;
        std::wstring text;
        std::vector<std::wstring> paths;
    };

    // 按 Win32 的 DROPFILES 布局拼一份内存：结构头 + 双 null 结尾的文件列表。
    //
    // 关键点（真实实现踩过坑）：
    //   pFiles 是「从结构体起始算起」的偏移，不是指针 —— 64 位下写成指针
    //   会让 DragQueryFileW 读到野内存；
    //   fWide = TRUE 时文件列表是 UTF-16，且必须以额外的 L'\0' 收尾。
    static HGLOBAL buildDropFiles(const std::vector<std::wstring>& paths) {
        std::size_t textBytes = 0;
        for (const std::wstring& path : paths) {
            textBytes += (path.size() + 1) * sizeof(wchar_t);
        }
        textBytes += sizeof(wchar_t);  // 外层再补一个结尾 null

        const std::size_t headerBytes = sizeof(DROPFILES);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                     headerBytes + textBytes);
        if (handle == nullptr) {
            return nullptr;
        }

        auto* header = static_cast<DROPFILES*>(GlobalLock(handle));
        if (header == nullptr) {
            GlobalFree(handle);
            return nullptr;
        }
        header->pFiles = static_cast<DWORD>(headerBytes);
        header->pt.x = 0;
        header->pt.y = 0;
        header->fNC = FALSE;
        header->fWide = TRUE;

        auto* cursor = reinterpret_cast<wchar_t*>(
            reinterpret_cast<std::byte*>(header) + headerBytes);
        for (const std::wstring& path : paths) {
            std::memcpy(cursor, path.c_str(),
                        (path.size() + 1) * sizeof(wchar_t));
            cursor += path.size() + 1;
        }
        *cursor = L'\0';

        GlobalUnlock(handle);
        return handle;
    }

    LONG refCount_ = 1;
    std::vector<Entry> wideTexts_;
    std::vector<Entry> paths_;
};

// ---------------------------------------------------------------- 用例

static void testDroppedPaths() {
    FakeDataObject data;
    data.setPaths(CF_HDROP,
                  {L"C:\\Windows\\notepad.exe", L"D:\\资料\\报表.xlsx"});

    CHECK(canAcceptDrop(&data));
    CHECK(countDroppedSources(&data) == 2);

    const std::vector<DroppedSource> sources = extractDroppedSources(&data);
    CHECK(sources.size() == 2);
    if (sources.size() == 2) {
        CHECK(sources[0].kind == DroppedKind::Path);
        CHECK(sources[0].value == L"C:\\Windows\\notepad.exe");
        CHECK(sources[1].kind == DroppedKind::Path);
        CHECK(sources[1].value == L"D:\\资料\\报表.xlsx");
    }
}

static void testSingleFolder() {
    FakeDataObject data;
    data.setPaths(CF_HDROP, {L"C:\\Users\\George\\项目空间"});

    const std::vector<DroppedSource> sources = extractDroppedSources(&data);
    CHECK(sources.size() == 1);
    if (sources.size() == 1) {
        CHECK(sources[0].kind == DroppedKind::Path);
        CHECK(sources[0].value == L"C:\\Users\\George\\项目空间");
    }
}

static void testUrlFormatWide() {
    FakeDataObject data;
    const CLIPFORMAT urlFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    data.setWideText(urlFormat, L"https://www.qq.com/index.html");

    CHECK(canAcceptDrop(&data));
    CHECK(countDroppedSources(&data) == 1);

    const std::vector<DroppedSource> sources = extractDroppedSources(&data);
    CHECK(sources.size() == 1);
    if (sources.size() == 1) {
        CHECK(sources[0].kind == DroppedKind::Url);
        CHECK(sources[0].value == L"https://www.qq.com/index.html");
    }
}

static void testPlainTextFallsBackToUrl() {
    FakeDataObject data;
    // 只给纯文本（有些来源真的只提供这个）
    data.setWideText(CF_UNICODETEXT, L"  https://example.com  ");

    CHECK(canAcceptDrop(&data));
    const std::vector<DroppedSource> sources = extractDroppedSources(&data);
    CHECK(sources.size() == 1);
    if (sources.size() == 1) {
        CHECK(sources[0].kind == DroppedKind::Url);
        // 首尾空白必须裁掉，否则会存进 JSON 并影响启动
        CHECK(sources[0].value == L"https://example.com");
    }
}

static void testPathsWinOverText() {
    FakeDataObject data;
    data.setWideText(CF_UNICODETEXT, L"https://should-be-ignored.example");
    data.setPaths(CF_HDROP, {L"C:\\a.exe"});

    // 同时有两类格式时，文件优先 —— 拖文件时资源管理器也会捎带文本，
    // 若让文本胜出就会把一次「拖 3 个文件」变成一个 URL 项目
    const std::vector<DroppedSource> sources = extractDroppedSources(&data);
    CHECK(sources.size() == 1);
    if (sources.size() == 1) {
        CHECK(sources[0].kind == DroppedKind::Path);
        CHECK(sources[0].value == L"C:\\a.exe");
    }
}

static void testUnsupportedFormat() {
    FakeDataObject data;
    const CLIPFORMAT bitmapFormat = static_cast<CLIPFORMAT>(CF_BITMAP);
    data.setWideText(bitmapFormat, L"");

    CHECK(!canAcceptDrop(&data));
    CHECK(countDroppedSources(&data) == 0);
    CHECK(extractDroppedSources(&data).empty());
}

static void testNullDataObject() {
    CHECK(!canAcceptDrop(nullptr));
    CHECK(countDroppedSources(nullptr) == 0);
    CHECK(extractDroppedSources(nullptr).empty());
}

static void testEmptyDropList() {
    FakeDataObject data;
    data.setPaths(CF_HDROP, {});

    // 有 CF_HDROP 格式 → 认（可以接受），但抠出来的条目是 0 个
    CHECK(canAcceptDrop(&data));
    CHECK(countDroppedSources(&data) == 0);
    CHECK(extractDroppedSources(&data).empty());
}

int wmain() {
    testDroppedPaths();
    testSingleFolder();
    testUrlFormatWide();
    testPlainTextFallsBackToUrl();
    testPathsWinOverText();
    testUnsupportedFormat();
    testNullDataObject();
    testEmptyDropList();

    if (g_failures == 0) {
        wprintf(L"===== DROP TARGET: ALL PASSED =====\n");
        return 0;
    }
    wprintf(L"===== DROP TARGET: %d FAILURES =====\n", g_failures);
    return g_failures;
}
