// KTile - 基础设施层：极简 JSON 写入器
//
// 为什么不用 nlohmann/json：
//   - 目标是「体积小、依赖少」，而 KTile 的 JSON 结构完全固定
//   - 只需覆盖写入与解析两个方向，手写约 400 行足够且零依赖
//
// 本文件只负责写入。解析在 json_parser.h。
//
// 设计要点：
//   - 流式写入：按 key/value 顺序调用，不需要先构建 DOM 树
//   - 缩进美化：写出的文件人眼可读、可手工编辑（配置文件必须可手改）
//   - 窄字符输出：JSON 规范允许 UTF-8，中文以 \uXXXX 转义或原样 UTF-8 均可。
//     这里选择原样 UTF-8（更小更可读），解析端按 UTF-8 处理。
//
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ktile {
namespace infrastructure {
namespace json {

// 流式 JSON 写入器。
//
// 用法：
//   JsonWriter writer;
//   writer.beginObject();
//   writer.key("version").value(1);
//   writer.key("tabs").beginArray();
//   writer.beginObject(); ... writer.endObject();
//   writer.endArray();
//   writer.endObject();
//   std::string text = writer.take();
//
class JsonWriter {
public:
    JsonWriter() { buffer_.reserve(4096); }

    // ---- 结构 ----
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    // ---- 键 ----
    JsonWriter& key(const std::string& name);

    // ---- 值 ----
    JsonWriter& value(const std::string& text);
    JsonWriter& value(const char* text);
    JsonWriter& value(bool flag);
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(std::uint64_t number);
    JsonWriter& value(std::uint32_t number);
    JsonWriter& value(double number);
    JsonWriter& nullValue();

    // 整数统一入口（供 member 模板调用）
    JsonWriter& valueInteger(long long signedValue,
                             unsigned long long unsignedValue,
                             bool negative);

    // ---- 便捷：一次写一个键值对 ----
    //
    // 整数统一用模板接住：任何整型 T 非负则走无符号，否则走有符号。
    // 这样避免 int/uint32_t/int64_t 多个重载互相二义。
    JsonWriter& member(const std::string& name, const std::string& text) {
        return key(name).value(text);
    }
    JsonWriter& member(const std::string& name, const char* text) {
        return key(name).value(text);
    }
    JsonWriter& member(const std::string& name, bool flag) {
        return key(name).value(flag);
    }
    JsonWriter& member(const std::string& name, double number) {
        return key(name).value(number);
    }

    template <typename Integer,
              typename = typename std::enable_if<
                  std::is_integral<Integer>::value &&
                  !std::is_same<Integer, bool>::value>::type>
    JsonWriter& member(const std::string& name, Integer number) {
        return key(name).valueInteger(static_cast<long long>(number),
                                      static_cast<unsigned long long>(number),
                                      number < 0);
    }

    // 取结果（转移所有权）
    std::string take() { return std::move(buffer_); }

    // 只读访问
    const std::string& text() const noexcept { return buffer_; }

    bool empty() const noexcept { return buffer_.empty(); }

private:
    // 写入前的「分隔符/换行/缩进」处理
    void prepareToken();
    // 追加缩进
    void appendIndent();
    // 转义并写入字符串（含首尾引号）
    void appendEscapedString(const std::string& text);

    std::string buffer_;

    // 嵌套状态栈：true = object，false = array
    std::vector<bool> stack_;
    // 当前层是否已写过至少一个元素（用于决定是否加逗号）
    std::vector<bool> hasElement_;

    // 上一个动作是 key()（表示紧接着要写一个值，而不是新元素）
    bool pendingKey_ = false;
};

}  // namespace json
}  // namespace infrastructure
}  // namespace ktile
