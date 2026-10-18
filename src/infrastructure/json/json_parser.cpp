// KTile - 基础设施层：极简 JSON 解析器实现
#include "infrastructure/json/json_parser.h"

#include <cstdlib>

namespace ktile {
namespace infrastructure {
namespace json {
namespace {

// 静态 Null 节点，供 find() 未命中时返回
const JsonValue& nullValueRef() {
    static const JsonValue instance = JsonValue::makeNull();
    return instance;
}

class Parser {
public:
    Parser(const std::string& text) : text_(text) {}

    ParseResult run() {
        ParseResult result;
        result.root = JsonValue::makeNull();

        skipWhitespace();
        if (atEnd()) {
            result.error = "输入为空";
            result.errorLine = line_;
            return result;
        }

        JsonValue root;
        if (!parseValue(root, 0)) {
            result.error = error_;
            result.errorLine = line_;
            return result;
        }

        skipWhitespace();
        if (!atEnd()) {
            result.error = "根值之后存在多余内容";
            result.errorLine = line_;
            return result;
        }

        result.ok = true;
        result.root = std::move(root);
        return result;
    }

private:
    static constexpr int kMaximumDepth = 64;

    bool atEnd() const noexcept { return index_ >= text_.size(); }

    char peek() const noexcept {
        return atEnd() ? '\0' : text_[index_];
    }

    char advance() {
        if (atEnd()) {
            return '\0';
        }
        const char ch = text_[index_++];
        if (ch == '\n') {
            ++line_;
        }
        return ch;
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char ch = peek();
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                advance();
            } else {
                break;
            }
        }
    }

    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > kMaximumDepth) {
            return fail("嵌套过深");
        }
        skipWhitespace();
        if (atEnd()) {
            return fail("值缺失");
        }

        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string text;
                if (!parseString(text)) {
                    return false;
                }
                out = JsonValue::makeString(std::move(text));
                return true;
            }
            case 't':
                if (matchLiteral("true")) {
                    out = JsonValue::makeBool(true);
                    return true;
                }
                return fail("非法字面量（期望 true）");
            case 'f':
                if (matchLiteral("false")) {
                    out = JsonValue::makeBool(false);
                    return true;
                }
                return fail("非法字面量（期望 false）");
            case 'n':
                if (matchLiteral("null")) {
                    out = JsonValue::makeNull();
                    return true;
                }
                return fail("非法字面量（期望 null）");
            default:
                return parseNumber(out);
        }
    }

    bool matchLiteral(const char* literal) {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text_.compare(index_, length, literal) != 0) {
            return false;
        }
        index_ += length;
        return true;
    }

    bool parseObject(JsonValue& out, int depth) {
        advance();  // '{'
        out = JsonValue::makeObject();

        skipWhitespace();
        if (peek() == '}') {
            advance();
            return true;
        }

        for (;;) {
            skipWhitespace();
            if (peek() != '"') {
                return fail("对象的键必须是字符串");
            }
            std::string name;
            if (!parseString(name)) {
                return false;
            }

            skipWhitespace();
            if (peek() != ':') {
                return fail("键之后缺少冒号");
            }
            advance();

            JsonValue child;
            if (!parseValue(child, depth + 1)) {
                return false;
            }
            out.setMember(std::move(name), std::move(child));

            skipWhitespace();
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == '}') {
                advance();
                return true;
            }
            return fail("对象缺少 , 或 }");
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        advance();  // '['
        out = JsonValue::makeArray();

        skipWhitespace();
        if (peek() == ']') {
            advance();
            return true;
        }

        for (;;) {
            JsonValue child;
            if (!parseValue(child, depth + 1)) {
                return false;
            }
            out.appendItem(std::move(child));

            skipWhitespace();
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == ']') {
                advance();
                return true;
            }
            return fail("数组缺少 , 或 ]");
        }
    }

    bool parseString(std::string& out) {
        advance();  // 开引号
        out.clear();

        for (;;) {
            if (atEnd()) {
                return fail("字符串未闭合");
            }
            const char ch = advance();
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out += ch;
                continue;
            }

            // 转义序列
            if (atEnd()) {
                return fail("转义序列未完成");
            }
            const char escaped = advance();
            switch (escaped) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (!parseUnicodeEscape(out)) {
                        return false;
                    }
                    break;
                }
                default:
                    return fail("未知的转义字符");
            }
        }
    }

    // \uXXXX -> UTF-8（按 BMP 码点直接编码，不合并代理对）
    bool parseUnicodeEscape(std::string& out) {
        unsigned int codePoint = 0;
        for (int i = 0; i < 4; ++i) {
            if (atEnd()) {
                return fail("\\u 转义不完整");
            }
            const char ch = advance();
            codePoint <<= 4;
            if (ch >= '0' && ch <= '9') {
                codePoint |= static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                codePoint |= static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                codePoint |= static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return fail("\\u 转义包含非十六进制字符");
            }
        }
        appendUtf8(out, codePoint);
        return true;
    }

    static void appendUtf8(std::string& out, const unsigned int codePoint) {
        if (codePoint < 0x80) {
            out += static_cast<char>(codePoint);
        } else if (codePoint < 0x800) {
            out += static_cast<char>(0xC0 | (codePoint >> 6));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (codePoint >> 12));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
    }

    bool parseNumber(JsonValue& out) {
        const std::size_t start = index_;
        if (peek() == '-' || peek() == '+') {
            advance();
        }
        bool hasDigits = false;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            advance();
            hasDigits = true;
        }
        if (!atEnd() && peek() == '.') {
            advance();
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                advance();
                hasDigits = true;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (peek() == '-' || peek() == '+') {
                advance();
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                advance();
            }
        }

        if (!hasDigits) {
            return fail("非法数字格式");
        }

        const std::string token = text_.substr(start, index_ - start);
        out = JsonValue::makeNumber(std::strtod(token.c_str(), nullptr));
        return true;
    }

    const std::string& text_;
    std::size_t index_ = 0;
    std::size_t line_ = 1;
    std::string error_;
};

}  // namespace

// ================================================================ JsonValue

bool JsonValue::asBool(const bool fallback) const noexcept {
    if (type_ == Type::Bool) {
        return bool_;
    }
    if (type_ == Type::Number) {
        return number_ != 0.0;
    }
    return fallback;
}

std::int64_t JsonValue::asInt(const std::int64_t fallback) const noexcept {
    if (type_ == Type::Number) {
        return static_cast<std::int64_t>(number_);
    }
    if (type_ == Type::Bool) {
        return bool_ ? 1 : 0;
    }
    return fallback;
}

std::uint32_t JsonValue::asUint32(const std::uint32_t fallback) const noexcept {
    const std::int64_t value = asInt(-1);
    if (value < 0 || value > 0xFFFFFFFFLL) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

double JsonValue::asDouble(const double fallback) const noexcept {
    if (type_ == Type::Number) {
        return number_;
    }
    return fallback;
}

const std::string& JsonValue::asString() const noexcept {
    if (type_ == Type::String) {
        return string_;
    }
    static const std::string empty;
    return empty;
}

const JsonValue& JsonValue::at(const std::size_t index) const noexcept {
    if (type_ == Type::Array && index < array_.size()) {
        return array_[index];
    }
    return nullValueRef();
}

const JsonValue& JsonValue::find(const std::string& name) const noexcept {
    if (type_ == Type::Object) {
        for (const auto& entry : object_) {
            if (entry.first == name) {
                return entry.second;
            }
        }
    }
    return nullValueRef();
}

bool JsonValue::hasKey(const std::string& name) const noexcept {
    if (type_ != Type::Object) {
        return false;
    }
    for (const auto& entry : object_) {
        if (entry.first == name) {
            return true;
        }
    }
    return false;
}

JsonValue JsonValue::makeNull() {
    JsonValue value;
    value.type_ = Type::Null;
    return value;
}

JsonValue JsonValue::makeBool(const bool flag) {
    JsonValue value;
    value.type_ = Type::Bool;
    value.bool_ = flag;
    return value;
}

JsonValue JsonValue::makeNumber(const double number) {
    JsonValue value;
    value.type_ = Type::Number;
    value.number_ = number;
    return value;
}

JsonValue JsonValue::makeString(std::string text) {
    JsonValue value;
    value.type_ = Type::String;
    value.string_ = std::move(text);
    return value;
}

JsonValue JsonValue::makeArray() {
    JsonValue value;
    value.type_ = Type::Array;
    return value;
}

JsonValue JsonValue::makeObject() {
    JsonValue value;
    value.type_ = Type::Object;
    return value;
}

void JsonValue::appendItem(JsonValue item) {
    if (type_ == Type::Array) {
        array_.push_back(std::move(item));
    }
}

void JsonValue::setMember(std::string name, JsonValue item) {
    if (type_ != Type::Object) {
        return;
    }
    for (auto& entry : object_) {
        if (entry.first == name) {
            entry.second = std::move(item);
            return;
        }
    }
    object_.emplace_back(std::move(name), std::move(item));
}

// ================================================================ 入口

ParseResult parseJson(const std::string& text) {
    Parser parser(text);
    return parser.run();
}

}  // namespace json
}  // namespace infrastructure
}  // namespace ktile
