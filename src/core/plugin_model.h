#pragma once

// 插件领域模型
//
// 设计取舍：
//   - 插件不是可执行脚本，而是 data/plugins/<id>/plugin.json 的声明式清单。
//     解析 JSON 属于 infrastructure 的职责，这里只保留与格式无关的模型，
//     保证 core 仍然只依赖标准库、可以脱离 Win32 做单元测试。
//   - 数据统一表示成 PluginValue（轻量 JSON 树），而不是字符串/字符串映射：
//     插件返回的数组和嵌套对象（天气 3 天预报、系统指标列表）需要保持结构，
//     repeat 布局才能在渲染期按数组长度动态展开。
//   - 布局是受限的两层树：容器（row/column/repeat）+ 叶子（slots）。
//     禁止像素坐标/字号/任意画布，避免插件把磁贴画成不可控的界面。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ktile {
namespace core {

// ================================================================ 通用值

struct PluginValue {
    enum class Type : std::uint8_t {
        Null = 0,
        Bool = 1,
        Number = 2,
        String = 3,
        Array = 4,
        Object = 5,
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<PluginValue> elements;
    std::vector<std::pair<std::string, PluginValue>> members;

    PluginValue() = default;
    PluginValue(const PluginValue&) = default;
    PluginValue& operator=(const PluginValue&) = default;
    PluginValue(PluginValue&&) = default;
    PluginValue& operator=(PluginValue&&) = default;

    static PluginValue makeNull();
    static PluginValue makeBool(bool value);
    static PluginValue makeNumber(double value);
    static PluginValue makeString(std::string value);
    static PluginValue makeArray();
    static PluginValue makeObject();

    bool isNull() const noexcept { return type == Type::Null; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isObject() const noexcept { return type == Type::Object; }
    bool isNumber() const noexcept { return type == Type::Number; }

    // 对象成员查找（不存在返回 nullptr）
    const PluginValue* find(const std::string& name) const noexcept;
    // 数组下标（越界返回 nullptr）
    const PluginValue* at(std::size_t index) const noexcept;
    std::size_t elementCount() const noexcept { return elements.size(); }

    // 面向界面显示的字符串：数字去掉多余的 0，布尔转 true/false。
    std::string displayString() const;
};

// ================================================================ 插件声明

enum class PluginSourceKind : std::uint8_t {
    Builtin = 0,
    Http = 1,
    Command = 2,
};

struct PluginSourceSpec {
    PluginSourceKind kind = PluginSourceKind::Builtin;

    // builtin：provider 名称（v1 只有 "system"）
    std::string provider;

    // http
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutSeconds = 8;

    // command
    std::string program;
    std::vector<std::string> arguments;
    // 相对插件目录的路径；为空表示只读 stdout
    std::string outputFile;

    PluginSourceSpec() = default;
};

struct PluginMeterSpec {
    std::string value;   // 模板，解析成 0..max 的数字
    std::string label;   // 模板
    std::string color;   // 模板，解析成 #RRGGBB；为空用主题强调色
    double maximum = 100.0;
};

// 布局叶子：最多 5 个语义槽位。
struct PluginSlotsSpec {
    std::string icon;    // 模板，结果是一个图标 id（如 "sun"）
    std::string main;    // 模板
    std::string sub;     // 模板
    std::string footer;  // 模板
    bool hasMeter = false;
    PluginMeterSpec meter;
};

struct PluginLayoutNode {
    enum class Kind : std::uint8_t {
        Leaf = 0,
        Container = 1,
    };

    Kind kind = Kind::Leaf;
    int weight = 0;          // 容器内分配权重，<=0 按 1 处理
    bool horizontal = false; // 容器方向：true=row，false=column

    // Leaf
    PluginSlotsSpec slots;

    // Container
    std::vector<PluginLayoutNode> children;

    // repeat：把 children 按数据数组重复展开
    bool hasRepeat = false;
    std::string repeatSource;  // 相对 select 路径，如 "days"
    std::string repeatMap;     // 循环变量名，如 "day"
    int repeatMax = 1;

    PluginLayoutNode() = default;
};

struct PluginLayoutSpec {
    PluginLayoutNode root;
    PluginLayoutSpec() = default;
};

struct PluginManifest {
    std::string id;
    std::string name;
    std::string description;
    std::string icon;          // 缺省图标（lucide id）
    int refreshSeconds = 300;

    // 面板可见时的高频刷新间隔（秒）。0 = 不随面板显示自动刷新，
    // 只在唤出面板 / 切换标签页时按 refreshSeconds 的 TTL 刷新一次。
    // 系统监控这类本地指标设成 1，面板开着时每秒更新；
    // 天气这类有配额/延迟的数据保持 0，避免长时间开着面板时刷爆接口。
    int visibleRefreshSeconds = 0;

    PluginSourceSpec source;

    // select：命名空间 → 数据路径。绑定里写 ${now.temp}，
    // 实际解析为 root.current.temp。
    std::vector<std::pair<std::string, std::string>> selectors;

    // icons：天气代码等原始值 → 图标 id 的纯数据映射。
    // 用它替代脚本里的 if/else，保持清单声明式。
    std::vector<std::pair<std::string, std::string>> iconMap;

    // layouts：键为 default/small/wide/tall/large，按磁贴尺寸自动选择。
    std::vector<std::pair<std::string, PluginLayoutSpec>> layouts;

    // 由 infrastructure 填充：插件目录绝对路径（仅运行期使用，不写 JSON）
    std::string directory;

    bool valid = false;
    std::string error;

    const PluginLayoutSpec* layoutForSize(int tileWidth, int tileHeight) const noexcept;
    const PluginLayoutSpec* layoutByKey(const std::string& key) const noexcept;
    const std::string* iconForRawValue(const std::string& raw) const noexcept;

    PluginManifest() = default;
};

// 磁贴尺寸到布局键的映射（与 ui 的 TileSize 语义一致，但 core 不依赖 ui）。
// 64x64=small，128x64=wide，64x128=tall，128x128=large。
const PluginLayoutSpec* selectPluginLayout(const PluginManifest& manifest,
                                           int tileWidth, int tileHeight) noexcept;

// ================================================================ 运行结果

struct PluginSnapshot {
    PluginValue data;
    std::int64_t updatedAt = 0;  // Unix 秒
    bool hasData = false;
    std::string error;

    PluginSnapshot() = default;
};

// UI 绘制前拿到的一份不可变视图。
// 用 shared_ptr 发布：刷新线程只创建新对象、不修改已发布对象，
// UI 绘制时拿到的指针对整个帧都是稳定的。
struct PluginView {
    std::shared_ptr<const PluginManifest> manifest;
    std::shared_ptr<const PluginSnapshot> snapshot;
    std::string error;
    bool hasData = false;
    bool loading = false;

    PluginView() = default;
};

struct PluginCatalogEntry {
    std::string id;
    std::string name;
    std::string description;

    PluginCatalogEntry() = default;
    PluginCatalogEntry(std::string entryId, std::string entryName,
                       std::string entryDescription)
        : id(std::move(entryId)),
          name(std::move(entryName)),
          description(std::move(entryDescription)) {}
};

}  // namespace core
}  // namespace ktile
