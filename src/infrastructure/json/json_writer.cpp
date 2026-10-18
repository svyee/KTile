// KTile - 基础设施层：极简 JSON 写入器实现
#include "infrastructure/json/json_writer.h"

#include <cmath>
#include <cstdio>

namespace ktile {
namespace infrastructure {
namespace json {
namespace {

constexpr const char* kIndentUnit = "  ";  // 两个空格
constexpr int kMaximumDepth = 64;          // 防御性深度上限

}  // namespace

void JsonWriter::appendIndent() {
    for (std::size_t i = 0; i < stack_.size(); ++i) {
        buffer_ += kIndentUnit;
    }
}

void JsonWriter::prepareToken() {
    // 紧接在 key() 之后写入的值：键已经写在当前行（含 ": "），
    // 不需要逗号，也不需要换行缩进。
    if (pendingKey_) {
        pendingKey_ = false;
        return;
    }

    if (stack_.empty()) {
        return;  // 顶层，无需分隔
    }

    if (hasElement_.back()) {
        buffer_ += ',';
    } else {
        hasElement_.back() = true;
    }
    buffer_ += '\n';
    appendIndent();
}

JsonWriter& JsonWriter::beginObject() {
    if (stack_.size() >= static_cast<std::size_t>(kMaximumDepth)) {
        return *this;  // 超深直接忽略，避免栈爆炸
    }
    prepareToken();
    buffer_ += '{';
    stack_.push_back(true);
    hasElement_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    if (stack_.empty() || !stack_.back()) {
        return *this;  // 栈不匹配，忽略
    }
    const bool hadElements = hasElement_.back();
    stack_.pop_back();
    hasElement_.pop_back();
    if (hadElements) {
        buffer_ += '\n';
        appendIndent();
    }
    buffer_ += '}';
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    if (stack_.size() >= static_cast<std::size_t>(kMaximumDepth)) {
        return *this;
    }
    prepareToken();
    buffer_ += '[';
    stack_.push_back(false);
    hasElement_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    if (stack_.empty() || stack_.back()) {
        return *this;
    }
    const bool hadElements = hasElement_.back();
    stack_.pop_back();
    hasElement_.pop_back();
    if (hadElements) {
        buffer_ += '\n';
        appendIndent();
    }
    buffer_ += ']';
    return *this;
}

JsonWriter& JsonWriter::key(const std::string& name) {
    // 键的定位在这里处理：上一个键值对写完后，新键需要前置逗号 + 换行缩进。
    // （值写入时因 pendingKey_ 为真而跳过，因此一行内是 "key": value 的形态。）
    if (stack_.empty()) {
        return *this;  // 顶层出现键属调用错误，忽略
    }
    if (hasElement_.back()) {
        buffer_ += ',';
    } else {
        hasElement_.back() = true;
    }
    buffer_ += '\n';
    appendIndent();

    pendingKey_ = true;
    appendEscapedString(name);
    buffer_ += ": ";
    return *this;
}

void JsonWriter::appendEscapedString(const std::string& text) {
    buffer_ += '"';
    for (const char raw : text) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"':  buffer_ += "\\\""; break;
            case '\\': buffer_ += "\\\\"; break;
            case '\b': buffer_ += "\\b";  break;
            case '\f': buffer_ += "\\f";  break;
            case '\n': buffer_ += "\\n";  break;
            case '\r': buffer_ += "\\r";  break;
            case '\t': buffer_ += "\\t";  break;
            default:
                if (ch < 0x20) {
                    // 其余控制字符用 \u00XX
                    char escaped[8] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04X", ch);
                    buffer_ += escaped;
                } else {
                    // >= 0x80 的字节原样输出（UTF-8 序列）
                    buffer_ += static_cast<char>(ch);
                }
                break;
        }
    }
    buffer_ += '"';
}

JsonWriter& JsonWriter::value(const std::string& text) {
    prepareToken();
    appendEscapedString(text);
    return *this;
}

JsonWriter& JsonWriter::value(const char* text) {
    return value(std::string(text != nullptr ? text : ""));
}

JsonWriter& JsonWriter::value(const bool flag) {
    prepareToken();
    buffer_ += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(const std::int64_t number) {
    prepareToken();
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(number));
    buffer_ += text;
    return *this;
}

JsonWriter& JsonWriter::value(const std::uint64_t number) {
    prepareToken();
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%llu",
                  static_cast<unsigned long long>(number));
    buffer_ += text;
    return *this;
}

JsonWriter& JsonWriter::value(const std::uint32_t number) {
    return value(static_cast<std::uint64_t>(number));
}

JsonWriter& JsonWriter::value(const double number) {
    prepareToken();
    if (!std::isfinite(number)) {
        buffer_ += "0";  // JSON 不支持 NaN/Inf
        return *this;
    }
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%.6g", number);
    buffer_ += text;
    return *this;
}

JsonWriter& JsonWriter::valueInteger(const long long signedValue,
                                     const unsigned long long unsignedValue,
                                     const bool negative) {
    prepareToken();
    char text[32] = {};
    if (negative) {
        std::snprintf(text, sizeof(text), "%lld", signedValue);
    } else {
        std::snprintf(text, sizeof(text), "%llu", unsignedValue);
    }
    buffer_ += text;
    return *this;
}

JsonWriter& JsonWriter::nullValue() {
    prepareToken();
    buffer_ += "null";
    return *this;
}

}  // namespace json
}  // namespace infrastructure
}  // namespace ktile
