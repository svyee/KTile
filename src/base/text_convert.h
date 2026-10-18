// KTile - C++17 兼容层：文本编码转换
//
// 单独提出头文件，因为 UI 层与持久化层都需要，但又不想把 span.h 里的
// Span 模板一起拖进来。
//
#pragma once

#include <string>

namespace ktile {
namespace base {

// UTF-8 <-> UTF-16（宽字符）互转。
// 失败（含非法字节序列）时返回空串，由调用方决定降级策略。
std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

// 宽松版本：转换失败时尽量返回已转换的部分，不抛异常。
// 用于用户输入等不可控来源。
std::wstring utf8ToWideLossy(const std::string& utf8);
std::string wideToUtf8Lossy(const std::wstring& wide);

}  // namespace base
}  // namespace ktile
