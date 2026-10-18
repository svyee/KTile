// KTile - 基础设施层：极简 JSON 解析器
//
// 面向 KTile 的固定结构，采用「按需取值」风格而非完整 DOM：
// 解析成轻量节点树，再按 key 查找。这样代码量小、可控、易调试。
//
// 支持：object / array / string / number / bool / null
// 不支持：注释、尾随逗号、单引号字符串、转义 \uD800 代理对合并
//        （配置文件中用不到，遇到即按原样处理）
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ktile {
namespace infrastructure {
namespace json {

class JsonValue;

// 轻量 JSON 节点
class JsonValue {
public:
    enum class Type : std::uint8_t {
        Null = 0,
        Bool = 1,
        Number = 2,
        String = 3,
        Array = 4,
        Object = 5,
    };

    JsonValue() = default;

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }

    // ---- 取值（类型不符时返回默认值，不抛异常）----
    bool asBool(bool fallback = false) const noexcept;
    std::int64_t asInt(std::int64_t fallback = 0) const noexcept;
    std::uint32_t asUint32(std::uint32_t fallback = 0) const noexcept;
    double asDouble(double fallback = 0.0) const noexcept;
    const std::string& asString() const noexcept;

    // ---- 数组 ----
    const std::vector<JsonValue>& items() const noexcept { return array_; }
    std::size_t size() const noexcept { return array_.size(); }
    const JsonValue& at(std::size_t index) const noexcept;

    // ---- 对象 ----
    // 按 key 查找。未找到返回一个静态 Null 节点（可安全链式调用）。
    const JsonValue& find(const std::string& name) const noexcept;
    bool hasKey(const std::string& name) const noexcept;
    const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
        return object_;
    }

    // ---- 构造（仅解析器使用）----
    static JsonValue makeNull();
    static JsonValue makeBool(bool value);
    static JsonValue makeNumber(double value);
    static JsonValue makeString(std::string value);
    static JsonValue makeArray();
    static JsonValue makeObject();

    void appendItem(JsonValue value);
    void setMember(std::string name, JsonValue value);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

// 解析结果
struct ParseResult {
    bool ok = false;
    JsonValue root;
    // 失败时的错误描述（含行号）
    std::string error;
    std::size_t errorLine = 0;
};

// 解析 UTF-8 JSON 文本
ParseResult parseJson(const std::string& text);

}  // namespace json
}  // namespace infrastructure
}  // namespace ktile
